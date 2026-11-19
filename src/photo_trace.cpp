#include "photo_trace.hpp"
#include "image_file.hpp"
#include "optics.hpp"
#include "optimizer.hpp"
#include "surface_mesh.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <thread>

namespace caustic {
namespace {
struct SceneInput {
    HeightField plate;
    LightSource light;
    TraceConfig cfg;
    std::vector<RectReceiver> screens;
};

SceneInput read_scene(const std::filesystem::path& settings) {
    std::ifstream in(settings);
    SceneInput scene;
    auto& [plate,light,cfg,screens] = scene;
    int area = 0, count = 0;
    auto vector = [&in](Vec3& p) { in >> p.x >> p.y >> p.z; };
    in >> plate.width_mm >> plate.thickness_mm >> plate.refractive_index >> cfg.ray_count >> area;
    plate.height_mm = plate.width_mm;
    vector(light.center);
    vector(light.u);
    vector(light.v);
    in >> light.width_mm >> light.height_mm >> count;
    if (!in || (area != 0 && area != 1) || count < 1 || count > 4096 ||
        cfg.ray_count < 1000 || cfg.ray_count > 200'000'000)
        throw std::invalid_argument("invalid CAD scene settings");
    light.kind = area ? LightSourceKind::Rectangle : LightSourceKind::Point;
    screens.resize(count);
    std::uint64_t pixels = 0;
    for (int i = 0; i < count; ++i) {
        auto& s = screens[i];
        s.id = std::to_string(i+1);
        vector(s.center);
        vector(s.u);
        vector(s.v);
        in >> s.width_mm >> s.height_mm >> s.image_width_px >> s.image_height_px;
        if (!in || s.image_width_px > 2048 || s.image_height_px > 2048)
            throw std::invalid_argument("invalid CAD receiver");
        pixels += std::uint64_t(s.image_width_px)*s.image_height_px;
    }
    if (!pixels || pixels > cfg.max_buffer_bytes/(2*sizeof(double)))
        throw std::invalid_argument("receiver images exceed memory budget");
    cfg.thread_count = std::min<std::uint64_t>(std::clamp(std::thread::hardware_concurrency(),1u,12u),
                                               cfg.max_buffer_bytes/(pixels*sizeof(double))-1);
    cfg.max_rays = cfg.ray_count;
    cfg.max_seconds = 240;
    cfg.stratify_entrance = true;
    return scene;
}
} // namespace

void fit_scene(const std::filesystem::path& input, const std::filesystem::path& out) {
    auto [plate,light,trace,screens] = read_scene(input/"scene.txt");
    std::ifstream angles(input/"angles.txt");
    int n, views;
    if (!(angles >> n >> views) || n<3 || n>15 || views<1 || views>32)
        throw std::invalid_argument("invalid fitting grid or number of views");
    std::vector<OptimizationFrame> frames(views);
    std::uint64_t pixels=0;
    for (int i=0;i<views;++i) {
        auto& frame = frames[i];
        if (!(angles >> frame.pose.rotation_y_deg) || !std::isfinite(frame.pose.rotation_y_deg))
            throw std::invalid_argument("invalid fitting angle");
        frame.receivers = screens;
        for (std::size_t k=0;k<screens.size();++k) {
            const auto& screen = screens[k];
            pixels += std::uint64_t(screen.image_width_px)*screen.image_height_px;
            if (pixels>1'000'000) throw std::invalid_argument("target frames exceed fitting budget");
            auto target=load_target_image(input/(std::to_string(i)+"_"+std::to_string(k)+".pgm"));
            if (target.width!=static_cast<int>(screen.image_width_px) || target.height!=static_cast<int>(screen.image_height_px))
                throw std::invalid_argument("incorrect target frame dimensions");
            frame.targets.push_back(std::move(target.pixels));
        }
    }
    plate.samples_x=plate.samples_y=n;
    plate.offsets_mm.assign(n*n,0.);
    plate.interpolation=HeightInterpolation::CubicLinearEnds;
    OptimizationConfig cfg;
    cfg.trace=trace;
    cfg.trace.seed=337903;
    cfg.max_seconds=120;
    cfg.max_evaluations=10*n*n;
    cfg.min_thickness_mm=.1*plate.thickness_mm;
    cfg.max_abs_offset_mm=.75*plate.thickness_mm;
    cfg.initial_step_mm=std::min(.25,cfg.max_abs_offset_mm/3);
    cfg.min_step_mm=cfg.initial_step_mm/16;
    const auto result=optimize_height_field(plate,light,frames,cfg);
    if (!result.evaluations || !std::isfinite(result.final_loss))
        throw std::runtime_error("no complete surface evaluation within budget");
    if (!std::filesystem::create_directory(out)) throw std::runtime_error("output directory already exists");
    std::ofstream surface(out/"surface.txt");
    surface << std::setprecision(17) << n << ' ' << plate.width_mm << ' ' << plate.thickness_mm << ' '
            << plate.refractive_index << ' ' << length(light.center) << ' ' << length(screens[0].center)
            << ' ' << screens[0].width_mm << ' ' << screens[0].height_mm << '\n';
    for (int j=0;j<n;++j) {
        for (int i=0;i<n;++i) surface << result.plate.offsets_mm[j*n+i] << ' ';
        surface << '\n';
    }
    surface.close();
    if (!surface) throw std::runtime_error("cannot save calculated optical surface");
    std::ofstream report(out/"fit.json");
    report << std::setprecision(12) << "{\"initial\":" << result.initial_loss << ",\"final\":" << result.final_loss
           << ",\"evaluations\":" << result.evaluations << ",\"seconds_limit\":"
           << (result.stop_reason==OptimizationStopReason::TimedOut ? "true" : "false") << "}";
    report.close();
    if (!report) throw std::runtime_error("cannot save fitting report");
}

void trace_scene(const std::filesystem::path& settings, const std::filesystem::path& profile,
                 const std::filesystem::path& out, double angle) {
    auto [plate,light,cfg,screens] = read_scene(settings);
    if (profile != "-") {
        std::ifstream file(profile);
        double width, thickness, design_index, source_z, screen_z, target_w, target_h;
        if (!(file >> plate.samples_x >> width >> thickness >> design_index >> source_z >> screen_z >> target_w >> target_h)
            || plate.samples_x < 3 || plate.samples_x > 1024 || std::abs(width-plate.width_mm)>1e-7 ||
            std::abs(thickness-plate.thickness_mm)>1e-7)
            throw std::invalid_argument("profile dimensions do not match the plate");
        plate.samples_y = plate.samples_x;
        plate.interpolation = HeightInterpolation::CubicLinearEnds;
        plate.offsets_mm.resize(std::size_t(plate.samples_x)*plate.samples_y);
        for (double& v : plate.offsets_mm)
            if (!(file >> v)) throw std::invalid_argument("incomplete optical profile");
        file >> std::ws;
        if (!file.eof()) throw std::invalid_argument("unexpected trailing profile data");
    }
    PlatePose pose;
    pose.rotation_y_deg=angle;
    auto result = trace_caustic(plate, pose, light, screens, cfg);
    if (result.stop_reason != TraceStopReason::Completed || result.stats.non_finite)
        throw std::runtime_error("incomplete CAD trace");
    if (!std::filesystem::create_directory(out)) throw std::runtime_error("output directory already exists");
    double peak = 0;
    for (const auto& img : result.images)
        peak = std::max(peak,*std::max_element(img.values.begin(),img.values.end()));
    std::ofstream report(out/"report.json");
    const auto& st = result.stats;
    report << std::setprecision(14) << "{\"n\":" << plate.refractive_index << ",\"rays\":" << st.processed
           << ",\"hit\":" << st.receiver_hit << ",\"tir\":" << st.total_internal_reflection
           << ",\"miss\":" << st.no_receiver << ",\"side\":" << st.side_escape
           << ",\"rejected\":" << st.source_rejected << ",\"peak\":" << peak << ",\"screens\":[";
    for (std::size_t i = 0; i < screens.size(); ++i) {
        const auto& img = result.images[i];
#ifdef _WIN32
        const std::string name = screens[i].id+".png";
#else
        const std::string name = screens[i].id+".pgm";
#endif
        save_projection_image(out/name,img.width,img.height,img.values,peak > 0 ? 1/peak : 1);
        const double pixel_area = screens[i].width_mm*screens[i].height_mm/(double(img.width)*img.height);
        report << (i ? "," : "") << "{\"image\":\"" << name << "\",\"flux\":"
               << pixel_area*std::accumulate(img.values.begin(),img.values.end(),0.) << "}";
    }
    report << "]}\n";
    report.close();
    if (!report) throw std::runtime_error("cannot save CAD trace report");
}

void trace_photo(const std::filesystem::path& profile, const std::filesystem::path& target_path,
                 const std::filesystem::path& out, int rays, int max_mesh_cells, int seed) {
    if (std::filesystem::exists(out)) throw std::invalid_argument("output directory already exists");
    HeightField plate;
    LightSource source;
    RectReceiver receiver;
    double source_distance, width, height;
    std::ifstream input(profile);
    if (!(input >> plate.samples_x >> plate.width_mm >> plate.thickness_mm >> plate.refractive_index
          >> source_distance >> receiver.center.z >> width >> height) ||
        plate.samples_x < 3 || plate.samples_x > 1024 || !(width > 0 && height > 0 && source_distance > 0)
        || rays < 1000 || rays > 200'000'000 || max_mesh_cells < 0 || max_mesh_cells > 2047)
        throw std::invalid_argument("invalid photo profile or ray/mesh budget");
    plate.interpolation = HeightInterpolation::CubicLinearEnds;
    plate.height_mm = plate.width_mm;
    plate.samples_y = plate.samples_x;
    plate.offsets_mm.resize(static_cast<std::size_t>(plate.samples_x)*plate.samples_y);
    for (auto& v : plate.offsets_mm)
        if (!(input >> v) || !std::isfinite(v)) throw std::invalid_argument("invalid spline coefficients");
    input >> std::ws;
    if (!input.eof()) throw std::invalid_argument("unexpected trailing coefficients");
    if (!(receiver.center.z > plate.thickness_mm + *std::max_element(plate.offsets_mm.begin(),plate.offsets_mm.end())))
        throw std::invalid_argument("screen intersects the plate");
    if (max_mesh_cells && max_mesh_cells < static_cast<int>(plate.samples_x)-1)
        throw std::invalid_argument("mesh budget smaller than the control lattice");
    const auto target = load_target_image(target_path);
    if (target.width > 1024 || target.height > 1024)
        throw std::invalid_argument("photo verification target exceeds 1024 pixels per side");
    const double target_sum = std::accumulate(target.pixels.begin(), target.pixels.end(), 0.);
    if (!(target_sum > 0)) throw std::invalid_argument("target flux must be positive");
    const int gx = (target.width + 7)/8, gy = (target.height + 7)/8;
    receiver.id = "screen";
    receiver.v = {0,-1,0};
    receiver.image_width_px = target.width + 2*gx;
    receiver.image_height_px = target.height + 2*gy;
    receiver.width_mm = width * receiver.image_width_px / target.width;
    receiver.height_mm = height * receiver.image_height_px / target.height;
    source.center.z = -source_distance;
    TraceConfig cfg;
    cfg.ray_count = cfg.max_rays = rays;
    cfg.thread_count = std::clamp(std::thread::hardware_concurrency(), 1u, 12u);
    cfg.max_seconds = 240;
    cfg.seed = seed;
    cfg.stratify_entrance = true;
    auto traced = trace_caustic(plate, {}, source, {receiver}, cfg);
    if (traced.stop_reason != TraceStopReason::Completed || traced.stats.non_finite)
        throw std::runtime_error("incomplete or unresolved photo trace");
    auto& image = traced.images.front();
    const double total = std::accumulate(image.values.begin(), image.values.end(), 0.);
    if (!(total > 0)) throw std::runtime_error("no flux landed on the verification screen");
    const double gain = target_sum / total;
    std::vector<double> full_target(image.values.size());
    double inside = 0, error = 0, norm = 0;
    for (int y = 0; y < target.height; ++y) {
        for (int x = 0; x < target.width; ++x) {
            const auto k = static_cast<std::size_t>(y+gy)*image.width + x+gx;
            const double t = target.pixels[static_cast<std::size_t>(y)*target.width+x];
            full_target[k] = t;
            inside += image.values[k];
            error += std::pow(gain*image.values[k]-t,2);
            norm += t*t;
        }
    }
    if (!out.parent_path().empty()) std::filesystem::create_directories(out.parent_path());
    auto staging = out;
    staging += ".partial-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if (!std::filesystem::create_directory(staging)) throw std::runtime_error("cannot reserve a new output directory");
    try {
        const auto save = [&](const char* name, const auto& pixels, double exposure) {
#ifdef _WIN32
            const char* ext = ".png";
#else
            const char* ext = ".pgm";
#endif
            save_projection_image(staging / (std::string(name)+ext),image.width,image.height,pixels,exposure);
        };
        save("01_monochrome",full_target,1);
        save("03_projection",image.values,gain);
        if (!max_mesh_cells) {
            // Calibration works in linear light, including peaks above display white.
            std::ofstream linear(staging/"irradiance.bin", std::ios::binary);
            for (double value : image.values) {
                const float irradiance = static_cast<float>(value*gain);
                linear.write(reinterpret_cast<const char*>(&irradiance), sizeof(irradiance));
            }
            linear.close();
            if (!linear) throw std::runtime_error("cannot save calibration irradiance");
        }
        std::filesystem::copy_file(profile,staging/"surface.txt");
        std::uint64_t triangles = 0;
        const unsigned subdivision = max_mesh_cells/(plate.samples_x-1);
        if (subdivision) {
            auto mesh = make_height_field_solid(plate,subdivision);
            std::string message;
            if (!save_binary_stl(staging/"02_plate.stl",mesh,message)) throw std::runtime_error(message);
            triangles = mesh.triangles.size();
        }
        std::ofstream report(staging/"report.json");
        report << std::setprecision(12)
               << "{\n  \"grid\": " << plate.samples_x << ", \"mesh_cells\": " << subdivision*(plate.samples_x-1)
               << ", \"triangles\": " << triangles << ",\n  \"picture_px\": [" << target.width << "," << target.height
               << "], \"guard_px\": [" << gx << "," << gy << "], \"picture_mm\": [" << width << "," << height
               << "],\n  \"rays\": " << traced.stats.processed << ", \"hit\": " << traced.stats.receiver_hit
               << ", \"side_escape\": " << traced.stats.side_escape << ", \"miss\": " << traced.stats.no_receiver
               << ", \"tir\": " << traced.stats.total_internal_reflection
               << ", \"unresolved\": " << traced.stats.non_finite << ",\n  \"captured_flux_guard_pct\": "
               << 100*(total-inside)/total << ", \"picture_relative_rmse\": " << std::sqrt(error/norm) << "\n}\n";
        report.close();
        if (!report) throw std::runtime_error("cannot save photo trace report");
        std::filesystem::rename(staging,out);
    } catch (...) {
        std::error_code cleanup;
        std::filesystem::remove_all(staging,cleanup);
        throw;
    }
}
}
