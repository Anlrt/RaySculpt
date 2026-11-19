#include "plate_design.hpp"

#include "diagnostics.hpp"
#include "image_file.hpp"
#include "optimizer.hpp"
#include "surface_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <numbers>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>

namespace caustic {
namespace {

struct ValidationFrame {
    TraceResult flat;
    TraceResult designed;
};

void validate_settings(const PlateDesign& s) {
    if (s.targets.empty() || s.targets.size() > 24 || s.output.empty())
        throw std::invalid_argument("provide 1 to 24 targets and a new --out directory");
    if (std::filesystem::exists(s.output))
        throw std::invalid_argument("--out already exists; choose a new directory");
    if (s.grid < 3 || s.grid > 25 || s.resolution < 8 || s.resolution > 96 ||
        s.rays < 1000 || s.rays > 1'000'000 || s.evaluations < 2 || s.evaluations > 5000 ||
        s.threads < 1 || s.threads > 64 || !(s.seconds >= 1 && s.seconds <= 600))
        throw std::invalid_argument("invalid grid, image resolution, or calculation budget");
    if (!(s.stl_height_mm > 0 && s.stl_normal_deg > 0 && s.stl_normal_deg <= 90))
        throw std::invalid_argument("STL height and normal tolerances must be positive");
    if (!(s.plate_mm > 0 && s.plate_mm <= 10'000 && s.source_mm > 0 && s.source_mm <= 1e6 &&
          s.thickness_mm > 0 && s.thickness_mm <= 1000 && s.max_relief_mm > 0 &&
          s.thickness_mm - s.max_relief_mm >= 0.5 && s.index > 1 && s.index <= 3 &&
          s.screen_mm > 0 && s.screen_mm <= 1e5 && s.screen_distance_mm <= 1e6 &&
          s.screen_distance_mm > s.thickness_mm + s.max_relief_mm))
        throw std::invalid_argument("invalid plate, source, or screen dimensions");
}

std::vector<OptimizationFrame> load_frames(const PlateDesign& s) {
    std::vector<OptimizationFrame> frames;
    double screen_aspect = 0;
    for (const auto& target : s.targets) {
        const double angle = std::remainder(target.angle_degrees, 360);
        if (!std::isfinite(angle) || std::abs(angle) >= 90)
            throw std::invalid_argument("point source cannot illuminate the entrance at angle " +
                                        std::to_string(target.angle_degrees));
        const auto image = load_target_image(target.image);
        const double aspect = static_cast<double>(image.width) / image.height;
        if (frames.empty()) screen_aspect = aspect;
        if (std::abs(aspect / screen_aspect - 1) > 0.01)
            throw std::invalid_argument("all images for this screen must have the same aspect ratio");
        const int width = std::max(1, static_cast<int>(std::lround(s.resolution * std::min(1.0, screen_aspect))));
        const int height = std::max(1, static_cast<int>(std::lround(s.resolution / std::max(1.0, screen_aspect))));
        auto pixels = resize_target(image, width, height).pixels;
        if (std::none_of(pixels.begin(), pixels.end(), [](double value) { return value > 0; }))
            throw std::invalid_argument("a target has no visible brightness");

        OptimizationFrame frame;
        frame.pose.rotation_y_deg = angle;
        auto& screen = frame.receivers.emplace_back();
        screen.id = "screen";
        screen.center.z = s.screen_distance_mm;
        screen.v = {0, -1, 0}; // Image row 0 corresponds to world +Y.
        screen.width_mm = s.screen_mm;
        screen.height_mm = s.screen_mm / screen_aspect;
        if (screen.height_mm > 1e5)
            throw std::invalid_argument("screen height derived from target aspect is too large");
        screen.image_width_px = width;
        screen.image_height_px = height;
        const double max_world_z = std::abs(std::sin(angle * std::numbers::pi / 180)) * s.plate_mm / 2 +
                                   std::cos(angle * std::numbers::pi / 180) *
                                   (s.thickness_mm + s.max_relief_mm);
        if (screen.center.z <= max_world_z)
            throw std::invalid_argument("screen is within the rotation envelope of the plate");
        frame.targets.push_back(std::move(pixels));
        frames.push_back(std::move(frame));
    }
    return frames;
}

double flux(std::span<const double> values) {
    return std::accumulate(values.begin(), values.end(), 0.0);
}

std::vector<double> paraxial_start(const PlateDesign& settings,
                                   const std::vector<OptimizationFrame>& frames) {
    // Estimate a quadratic relief that moves the illuminated footprint toward
    // the centre and spread of the target. Full tracing scores this trial.
    const auto frame = std::min_element(frames.begin(), frames.end(), [](const auto& a, const auto& b) {
        return std::abs(a.pose.rotation_y_deg) < std::abs(b.pose.rotation_y_deg);
    });
    if (std::abs(frame->pose.rotation_y_deg) > 25) return {};
    const auto& r = frame->receivers.front();
    const auto& pixels = frame->targets.front();
    const double total = flux(pixels);
    const double pixel_w = r.width_mm / r.image_width_px;
    const double pixel_h = r.height_mm / r.image_height_px;
    double mean_x = 0, mean_y = 0, xx = 0, xy = 0, yy = 0;
    for (std::uint32_t y = 0; y < r.image_height_px; ++y) {
        const double py = r.height_mm / 2 - (y + 0.5) * pixel_h;
        for (std::uint32_t x = 0; x < r.image_width_px; ++x) {
            const double px = (x + 0.5) * pixel_w - r.width_mm / 2;
            const double w = pixels[static_cast<std::size_t>(y) * r.image_width_px + x] / total;
            mean_x += w * px;
            mean_y += w * py;
            xx += w * px * px;
            xy += w * px * py;
            yy += w * py * py;
        }
    }
    xx = std::max(0.0, xx - mean_x * mean_x) + pixel_w * pixel_w / 12;
    yy = std::max(0.0, yy - mean_y * mean_y) + pixel_h * pixel_h / 12;
    xy -= mean_x * mean_y;
    const double root_det = std::sqrt(std::max(0.0, xx * yy - xy * xy));
    const double root_scale = std::sqrt(xx + yy + 2 * root_det) * settings.plate_mm / std::sqrt(12.0);
    const double after_plate = settings.screen_distance_mm - settings.thickness_mm;
    const double magnification = 1 + (after_plate + settings.thickness_mm / settings.index) / settings.source_mm;
    const double slope_gain = (settings.index - 1) * after_plate;
    const double axx = (xx + root_det) / root_scale - magnification;
    const double ayy = (yy + root_det) / root_scale - magnification;
    const double axy = xy / root_scale;
    std::vector<double> relief;
    relief.reserve(static_cast<std::size_t>(settings.grid) * settings.grid);
    for (int y = 0; y < settings.grid; ++y) {
        const double py = (static_cast<double>(y) / (settings.grid - 1) - 0.5) * settings.plate_mm;
        for (int x = 0; x < settings.grid; ++x) {
            const double px = (static_cast<double>(x) / (settings.grid - 1) - 0.5) * settings.plate_mm;
            relief.push_back((0.5 * (axx * px * px + ayy * py * py) + axy * px * py +
                              mean_x * px + mean_y * py) / slope_gain);
        }
    }
    const auto [low, high] = std::minmax_element(relief.begin(), relief.end());
    const double midpoint = (*high + *low) / 2;
    const double span = *high - *low;
    const double scale = span > 0 ? std::min(1.0, 1.8 * settings.max_relief_mm / span) : 1.0;
    for (auto& offset : relief) offset = (offset - midpoint) * scale;
    return relief;
}

double relative_error(std::span<const double> actual, std::span<const double> target, double gain) {
    double squared_error = 0;
    double squared_target = 0;
    for (std::size_t i = 0; i < target.size(); ++i) {
        const double delta = actual[i] * gain - target[i];
        squared_error += delta * delta;
        squared_target += target[i] * target[i];
    }
    return std::sqrt(squared_error / squared_target);
}

const char* stop_name(OptimizationStopReason reason) {
    switch (reason) {
    case OptimizationStopReason::Converged: return "step_limit";
    case OptimizationStopReason::Budget: return "evaluation_limit";
    case OptimizationStopReason::TimedOut: return "time_limit";
    case OptimizationStopReason::Cancelled: return "cancelled";
    }
    return "unknown";
}

} // namespace

void design_plate(const PlateDesign& settings) {
    validate_settings(settings);
    const auto frames = load_frames(settings);
    HeightField flat;
    flat.samples_x = flat.samples_y = settings.grid;
    flat.offsets_mm.assign(static_cast<std::size_t>(settings.grid) * settings.grid, 0);
    flat.width_mm = flat.height_mm = settings.plate_mm;
    flat.thickness_mm = settings.thickness_mm;
    flat.refractive_index = settings.index;
    flat.interpolation = HeightInterpolation::CubicSpline;
    LightSource source;
    source.center.z = -settings.source_mm;

    PreflightScene scene;
    scene.optic = make_height_field_solid(flat);
    scene.source = source;
    scene.receivers = frames.front().receivers;
    scene.angles_degrees.clear();
    for (const auto& frame : frames) scene.angles_degrees.push_back(frame.pose.rotation_y_deg);
    scene.rays_per_iteration = settings.rays;
    scene.iterations = settings.evaluations;
    const auto preflight = run_preflight(scene);
    for (const auto& diagnostic : preflight.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    if (!preflight.can_start()) throw std::invalid_argument("scene failed preflight");

    OptimizationConfig options;
    options.trace.ray_count = settings.rays;
    options.trace.thread_count = settings.threads;
    options.trace.max_seconds = settings.seconds;
    options.max_seconds = settings.seconds;
    options.max_evaluations = settings.evaluations;
    options.max_abs_offset_mm = settings.max_relief_mm;
    options.initial_step_mm = std::min(0.25, settings.max_relief_mm);
    options.min_step_mm = std::min(0.01, options.initial_step_mm);
    options.starting_relief_mm = paraxial_start(settings, frames);
    std::cerr << "Designing one plate for " << frames.size() << " target view(s)\n";
    const auto design = optimize_height_field(flat, source, frames, options);
    if (design.evaluations == 0 || !std::isfinite(design.final_loss))
        throw std::runtime_error("design stopped before the first evaluation completed");

    std::cerr << "Calculation stopped at " << stop_name(design.stop_reason) << " after "
              << design.evaluations << " evaluations. Validating with fresh rays.\n";
    TraceConfig validation_trace = options.trace;
    validation_trace.seed = 0x6ac3e529;
    validation_trace.ray_count = std::max<std::uint64_t>(600'000, options.trace.ray_count * 2);
    validation_trace.max_seconds = 10;
    std::vector<ValidationFrame> validation;
    double flat_flux = 0;
    double designed_flux = 0;
    double target_flux = 0;
    double target_peak = 0;
    for (const auto& frame : frames) {
        auto& check = validation.emplace_back();
        check.flat = trace_caustic(flat, frame.pose, source, frame.receivers, validation_trace);
        check.designed = trace_caustic(design.plate, frame.pose, source, frame.receivers, validation_trace);
        if (check.flat.stop_reason != TraceStopReason::Completed ||
            check.designed.stop_reason != TraceStopReason::Completed)
            throw std::runtime_error("validation trace did not complete");
        flat_flux += flux(check.flat.images.front().values);
        designed_flux += flux(check.designed.images.front().values);
        const auto& target = frame.targets.front();
        target_flux += flux(target);
        target_peak = std::max(target_peak, *std::max_element(target.begin(), target.end()));
    }
    if (!(designed_flux > 0))
        std::cerr << "NO_DESIGNED_FLUX: no validation rays reached any screen.\n";
    const double flat_gain = flat_flux > 0 ? target_flux / flat_flux : 0;
    const double designed_gain = designed_flux > 0 ? target_flux / designed_flux : 0;

    SplineTessellation tessellation{};
    Mesh mesh;
    if (settings.write_stl) {
        tessellation = tessellate_to_tolerance(design.plate, settings.stl_height_mm, settings.stl_normal_deg);
        mesh = make_height_field_solid(design.plate, tessellation.subdivisions);
    }
    if (!std::filesystem::create_directories(settings.output))
        throw std::runtime_error("output directory already exists");
    const auto log = settings.output / "diagnostics.jsonl";
    std::string error;
    if (!append_preflight_log(log, preflight, 1, error)) throw std::runtime_error(error);
    if (settings.write_stl && !save_binary_stl(settings.output / "plate.stl", mesh, error))
        throw std::runtime_error(error);

    std::ostringstream report;
    report.imbue(std::locale::classic());
    report << std::setprecision(12) << "{\n"
           << "  \"stop_reason\": \"" << stop_name(design.stop_reason) << "\",\n"
           << "  \"evaluations\": " << design.evaluations << ",\n"
           << "  \"initial_loss\": " << design.initial_loss << ",\n"
           << "  \"final_loss\": " << design.final_loss << ",\n"
           << "  \"grid\": " << settings.grid << ",\n"
           << "  \"surface\": \"uniform_cubic_b_spline\",\n"
           << "  \"stl_triangles\": " << mesh.triangles.size() << ",\n"
           << "  \"stl_height_error_bound_mm\": " << tessellation.height_error_mm << ",\n"
           << "  \"stl_normal_error_bound_deg\": " << tessellation.normal_error_deg << ",\n"
           << "  \"plate_mm\": " << settings.plate_mm << ",\n"
           << "  \"thickness_mm\": " << settings.thickness_mm << ",\n"
           << "  \"refractive_index\": " << settings.index << ",\n"
           << "  \"source_distance_mm\": " << settings.source_mm << ",\n"
           << "  \"screen_distance_mm\": " << settings.screen_distance_mm << ",\n"
           << "  \"screen_width_mm\": " << scene.receivers.front().width_mm << ",\n"
           << "  \"screen_height_mm\": " << scene.receivers.front().height_mm << ",\n"
           << "  \"validation_rays_per_trace\": " << validation_trace.ray_count << ",\n"
           << "  \"spline_coefficients_mm\": [";
    for (std::size_t i = 0; i < design.plate.offsets_mm.size(); ++i) {
        if (i != 0) report << ", ";
        report << design.plate.offsets_mm[i];
    }
    report << "],\n  \"views\": [\n";
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto& frame = frames[i];
        auto& check = validation[i];
        const auto& flat_image = check.flat.images.front();
        const auto& designed_image = check.designed.images.front();
        const auto& target = frame.targets.front();
        const double flat_error = relative_error(flat_image.values, target, flat_gain);
        const double designed_error = relative_error(designed_image.values, target, designed_gain);
        const std::string prefix = "view_" + std::to_string(i + 1);
        const auto save = [&](const char* name, std::span<const double> pixels, double gain) {
#ifdef _WIN32
            constexpr const char* extension = ".png";
#else
            constexpr const char* extension = ".pgm";
#endif
            save_projection_image(settings.output / (prefix + name + extension),
                                  flat_image.width, flat_image.height, pixels, gain / target_peak);
        };
        save("_target", target, 1);
        save("_flat", flat_image.values, flat_gain);
        save("_designed", designed_image.values, designed_gain);
        if (!append_trace_log(log, check.designed, 1, error)) throw std::runtime_error(error);
        std::cout << "View " << i + 1 << " (" << frame.pose.rotation_y_deg
                  << " deg), validation error: " << flat_error << " -> " << designed_error
                  << ", rays on screen: " << check.designed.stats.receiver_hit << '/'
                  << check.designed.stats.processed << '\n';
        if (designed_error >= flat_error)
            std::cerr << "NO_VALIDATED_IMPROVEMENT: view " << i + 1
                      << " did not improve over a flat plate.\n";
        if (check.designed.stats.non_finite)
            std::cerr << "UNRESOLVED_RAYS: view " << i + 1 << " has "
                      << check.designed.stats.non_finite << " unresolved paths.\n";
        report << "    {\"view\": " << i + 1 << ", \"angle_degrees\": " << frame.pose.rotation_y_deg
               << ", \"flat_error\": " << flat_error
               << ", \"designed_error\": " << designed_error
               << ", \"flat_flux\": " << flux(flat_image.values)
               << ", \"designed_flux\": " << flux(designed_image.values)
               << ", \"rays_on_screen\": " << check.designed.stats.receiver_hit
               << ", \"improved\": " << (designed_error < flat_error ? "true" : "false") << '}'
               << (i + 1 < frames.size() ? ",\n" : "\n");
    }
    report << "  ]\n}\n";
    std::ofstream report_file(settings.output / "report.json", std::ios::binary);
    report_file << report.str();
    report_file.close();
    if (!report_file) throw std::runtime_error("cannot complete design report");
}

} // namespace caustic
