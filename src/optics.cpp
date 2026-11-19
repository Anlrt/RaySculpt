#include "optics.hpp"
#include "height_surface.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <thread>

namespace caustic {
namespace {

constexpr double kEpsilon = 1e-10;
constexpr double kSurfaceOffset = 1e-7; // mm, avoids a receiver hit at the exit surface.

struct ExitHit {
    Vec3 position;
    HeightSample surface;
};

enum class ExitStatus { Hit, SideEscape, Invalid };

struct RayOutcome {
    enum class Kind { SourceRejected, SideEscape, InternalReflection, NoReceiver,
                      ReceiverHit, NonFinite } kind = Kind::NonFinite;
    std::size_t receiver = 0;
    std::size_t pixel = 0;
    double flux = 0.0;
};

struct ThreadState {
    std::vector<std::vector<double>> images;
    TraceStats stats;
};

bool positive_finite(double x) { return std::isfinite(x) && x > 0.0; }

double random01(std::uint64_t seed, std::uint64_t index, std::uint64_t dimension) {
    // SplitMix64: ray samples do not depend on the order in which threads run.
    std::uint64_t x = seed ^ (index * 0x9E3779B97F4A7C15ULL) ^
                      (dimension * 0xD1B54A32D192ED03ULL);
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return static_cast<double>(x >> 11) * 0x1.0p-53;
}

Vec3 rotate_y(Vec3 p, double c, double s) {
    return {c * p.x + s * p.z, p.y, -s * p.x + c * p.z};
}

Vec3 inverse_rotate_y(Vec3 p, double c, double s) {
    return {c * p.x - s * p.z, p.y, s * p.x + c * p.z};
}

bool refract(Vec3 incident, Vec3 normal_towards_incident, double n_in,
             double n_out, Vec3& transmitted, double& power_transmission) {
    const double cos_i = -dot(normal_towards_incident, incident);
    if (!(cos_i > 0.0 && cos_i <= 1.0 + 1e-8)) return false;
    const double eta = n_in / n_out;
    const double k = 1.0 - eta * eta * (1.0 - std::min(1.0, cos_i * cos_i));
    if (k < 0.0) return false;
    const double cos_t = std::sqrt(k);
    transmitted = normalize(eta * incident + (eta * cos_i - cos_t) * normal_towards_incident);
    if (!finite(transmitted) || length_squared(transmitted) < 0.99) return false;
    const double rs_denom = n_in * cos_i + n_out * cos_t;
    const double rp_denom = n_out * cos_i + n_in * cos_t;
    if (!(rs_denom > 0.0 && rp_denom > 0.0)) return false;
    const double rs = (n_in * cos_i - n_out * cos_t) / rs_denom;
    const double rp = (n_out * cos_i - n_in * cos_t) / rp_denom;
    power_transmission = std::clamp(1.0 - 0.5 * (rs * rs + rp * rp), 0.0, 1.0);
    return true;
}

std::size_t grid_index(const HeightField& plate, std::size_t ix, std::size_t iy) {
    return iy * plate.samples_x + ix;
}

HeightSample sample_height(const HeightField& plate, double x, double y) {
    const double fx = std::clamp((x / plate.width_mm + 0.5) * (plate.samples_x - 1),
                                 0.0, static_cast<double>(plate.samples_x - 1));
    const double fy = std::clamp((y / plate.height_mm + 0.5) * (plate.samples_y - 1),
                                 0.0, static_cast<double>(plate.samples_y - 1));
    const std::size_t ix = std::min<std::size_t>(static_cast<std::size_t>(fx), plate.samples_x - 2);
    const std::size_t iy = std::min<std::size_t>(static_cast<std::size_t>(fy), plate.samples_y - 2);
    const double u = fx - ix;
    const double v = fy - iy;
    const double h00 = plate.offsets_mm[grid_index(plate, ix, iy)];
    const double h10 = plate.offsets_mm[grid_index(plate, ix + 1, iy)];
    const double h01 = plate.offsets_mm[grid_index(plate, ix, iy + 1)];
    const double h11 = plate.offsets_mm[grid_index(plate, ix + 1, iy + 1)];
    HeightSample out;
    out.height = (1 - u) * (1 - v) * h00 + u * (1 - v) * h10 +
                 (1 - u) * v * h01 + u * v * h11;
    out.dx = ((1 - v) * (h10 - h00) + v * (h11 - h01)) *
             (plate.samples_x - 1) / plate.width_mm;
    out.dy = ((1 - u) * (h01 - h00) + u * (h11 - h10)) *
             (plate.samples_y - 1) / plate.height_mm;
    return out;
}

double earliest_quadratic_root(double a, double b, double c, double low, double high) {
    constexpr double tol = 1e-9;
    if (std::abs(a) < 1e-14) {
        if (std::abs(b) < 1e-14) return std::numeric_limits<double>::infinity();
        const double root = -c / b;
        return root >= low - tol && root <= high + tol ? root :
               std::numeric_limits<double>::infinity();
    }
    const double disc = b * b - 4.0 * a * c;
    if (disc < 0.0) return std::numeric_limits<double>::infinity();
    const double sqrt_disc = std::sqrt(std::max(0.0, disc));
    const double q = -0.5 * (b + std::copysign(sqrt_disc, b));
    const double root1 = q / a;
    const double root2 = std::abs(q) > 1e-30 ? c / q : -b / (2.0 * a);
    double best = std::numeric_limits<double>::infinity();
    if (root1 >= low - tol && root1 <= high + tol) best = root1;
    if (root2 >= low - tol && root2 <= high + tol) best = std::min(best, root2);
    return best;
}

ExitStatus intersect_exit(const HeightField& plate, Vec3 front, Vec3 direction, ExitHit& hit) {
    // Within one grid cell, bilinear height evaluated along the ray is a
    // quadratic in ray distance. Walking cells finds the first intersection,
    // including surfaces for which simple fixed-point iteration would fail.
    if (!(direction.z > kEpsilon) || !finite(direction)) return ExitStatus::Invalid;
    const double half_w = plate.width_mm * 0.5;
    const double half_h = plate.height_mm * 0.5;
    const double step_x = plate.width_mm / (plate.samples_x - 1);
    const double step_y = plate.height_mm / (plate.samples_y - 1);
    double side_t = std::numeric_limits<double>::infinity();
    if (direction.x > kEpsilon) side_t = std::min(side_t, (half_w - front.x) / direction.x);
    if (direction.x < -kEpsilon) side_t = std::min(side_t, (-half_w - front.x) / direction.x);
    if (direction.y > kEpsilon) side_t = std::min(side_t, (half_h - front.y) / direction.y);
    if (direction.y < -kEpsilon) side_t = std::min(side_t, (-half_h - front.y) / direction.y);
    if (!(side_t > 0.0)) return ExitStatus::SideEscape;

    double t = 0.0;
    const std::size_t max_steps = static_cast<std::size_t>(plate.samples_x) + plate.samples_y + 4;
    for (std::size_t step = 0; step < max_steps; ++step) {
        const double probe_t = t + std::min(1e-8, (side_t - t) * 0.5);
        const double px = front.x + probe_t * direction.x;
        const double py = front.y + probe_t * direction.y;
        if (px < -half_w - 1e-8 || px > half_w + 1e-8 ||
            py < -half_h - 1e-8 || py > half_h + 1e-8) return ExitStatus::SideEscape;
        const std::size_t ix = std::min<std::size_t>(
            static_cast<std::size_t>(std::clamp((px + half_w) / step_x, 0.0,
                                                static_cast<double>(plate.samples_x - 1))),
            plate.samples_x - 2);
        const std::size_t iy = std::min<std::size_t>(
            static_cast<std::size_t>(std::clamp((py + half_h) / step_y, 0.0,
                                                static_cast<double>(plate.samples_y - 1))),
            plate.samples_y - 2);
        const double x0 = -half_w + ix * step_x;
        const double y0 = -half_h + iy * step_y;
        const double h00 = plate.offsets_mm[grid_index(plate, ix, iy)];
        const double h10 = plate.offsets_mm[grid_index(plate, ix + 1, iy)];
        const double h01 = plate.offsets_mm[grid_index(plate, ix, iy + 1)];
        const double h11 = plate.offsets_mm[grid_index(plate, ix + 1, iy + 1)];
        const double a0 = h00;
        const double bu = h10 - h00;
        const double cv = h01 - h00;
        const double duv = h11 - h10 - h01 + h00;
        const double u0 = (front.x - x0) / step_x;
        const double v0 = (front.y - y0) / step_y;
        const double ut = direction.x / step_x;
        const double vt = direction.y / step_y;
        const double qa = -duv * ut * vt;
        const double qb = direction.z - bu * ut - cv * vt - duv * (u0 * vt + v0 * ut);
        const double qc = -plate.thickness_mm - a0 - bu * u0 - cv * v0 - duv * u0 * v0;

        double next_x = std::numeric_limits<double>::infinity();
        double next_y = std::numeric_limits<double>::infinity();
        if (direction.x > kEpsilon) next_x = (x0 + step_x - front.x) / direction.x;
        if (direction.x < -kEpsilon) next_x = (x0 - front.x) / direction.x;
        if (direction.y > kEpsilon) next_y = (y0 + step_y - front.y) / direction.y;
        if (direction.y < -kEpsilon) next_y = (y0 - front.y) / direction.y;
        const double end = std::min({next_x, next_y, side_t});
        if (!(end >= t) || !std::isfinite(t)) return ExitStatus::Invalid;
        const double root = earliest_quadratic_root(qa, qb, qc, t, end);
        if (std::isfinite(root)) {
            hit.position = front + direction * std::max(0.0, root);
            hit.surface = sample_height(plate, hit.position.x, hit.position.y);
            return finite(hit.position) && std::isfinite(hit.surface.height) ?
                   ExitStatus::Hit : ExitStatus::Invalid;
        }
        if (end >= side_t - 1e-9) return ExitStatus::SideEscape;
        if (!(end > t + 1e-13)) return ExitStatus::Invalid;
        t = end;
    }
    return ExitStatus::Invalid;
}

ExitStatus intersect_spline(const HeightField& plate, const SplineBounds& bounds,
                            Vec3 front, Vec3 direction, ExitHit& hit) {
    const double z = direction.z;
    if (!(z > kEpsilon) || !finite(direction)) return ExitStatus::Invalid;
    const auto boundary = [](double position, double speed, double width) {
        if (std::abs(speed) < kEpsilon) return std::numeric_limits<double>::infinity();
        return (std::copysign(width / 2, speed) - position) / speed;
    };
    const double side = std::min(boundary(front.x, direction.x, plate.width_mm),
                                 boundary(front.y, direction.y, plate.height_mm));
    if (bounds.min_z == bounds.max_z) {
        const double t = bounds.min_z / z;
        if (t >= side) return ExitStatus::SideEscape;
        hit = {front + t * direction, {bounds.min_z - plate.thickness_mm, 0, 0}};
        return ExitStatus::Hit;
    }
    constexpr double tolerance = 1e-8; // mm
    double left = std::max(0.0, (bounds.min_z - tolerance) / z);
    double right = std::min(side, (bounds.max_z + tolerance) / z);
    if (right <= left) return ExitStatus::SideEscape;
    const auto evaluate = [&](double t) {
        const Vec3 point = front + t * direction;
        const auto sample = sample_spline(plate, point.x, point.y);
        return std::pair{point.z - plate.thickness_mm - sample.height, sample};
    };
    const auto derivative = [&](const HeightSample& sample) {
        return z - sample.dx * direction.x - sample.dy * direction.y;
    };
    const double slope = bounds.dx * std::abs(direction.x) + bounds.dy * std::abs(direction.y);
    if (z > slope) {
        if (evaluate(right).first < 0) return ExitStatus::SideEscape;
    } else {
        // For grazing rays keep the earliest crossing. The second derivative
        // bounds allow us to discard intervals or certify a monotone bracket.
        const double curvature = bounds.dxx * direction.x * direction.x +
            2 * bounds.dxy * std::abs(direction.x * direction.y) +
            bounds.dyy * direction.y * direction.y;
        int work_left = 256;
        const auto first_bracket = [&](auto&& self, double a, double b, double fb) -> ExitStatus {
            if (--work_left < 0) return ExitStatus::Invalid;
            const double half = (b - a) / 2;
            const double mid = a + half;
            const auto [fm, sample] = evaluate(mid);
            const double slope_mid = derivative(sample);
            if (fm + std::abs(slope_mid) * half + 0.5 * curvature * half * half < -tolerance)
                return ExitStatus::SideEscape;
            if (slope_mid > curvature * half || b - a < tolerance) {
                if (fb < 0) return b - a < tolerance ? ExitStatus::Invalid : ExitStatus::SideEscape;
                left = a;
                right = b;
                return ExitStatus::Hit;
            }
            const auto first = self(self, a, mid, fm);
            return first != ExitStatus::SideEscape ? first : self(self, mid, b, fb);
        };
        const auto bracket = first_bracket(first_bracket, left, right, evaluate(right).first);
        if (bracket != ExitStatus::Hit) return bracket;
    }

    double t = (left + right) / 2;
    for (int i = 0; i < 48; ++i) {
        const auto [f, sample] = evaluate(t);
        const double slope_at_t = derivative(sample);
        if (right - left <= tolerance || std::abs(f) <= tolerance * std::max(slope_at_t, 0.0)) {
            hit = {front + t * direction, sample};
            return ExitStatus::Hit;
        }
        if (f < 0) left = t;
        else right = t;
        const double next = slope_at_t > kEpsilon ? t - f / slope_at_t : left;
        t = next > left && next < right ? next : (left + right) / 2;
    }
    return ExitStatus::Invalid;
}

bool receiver_intersection(const RectReceiver& receiver, Vec3 origin, Vec3 direction,
                           double& ray_t, double& normalized_u, double& normalized_v) {
    const Vec3 normal = cross(receiver.u, receiver.v);
    const double denominator = dot(normal, direction);
    if (std::abs(denominator) < kEpsilon) return false;
    const double t = dot(receiver.center - origin, normal) / denominator;
    if (!(t > kSurfaceOffset) || !std::isfinite(t)) return false;
    const Vec3 delta = origin + direction * t - receiver.center;
    const double u = dot(delta, receiver.u) / receiver.width_mm;
    const double v = dot(delta, receiver.v) / receiver.height_mm;
    if (std::abs(u) > 0.5 || std::abs(v) > 0.5) return false;
    ray_t = t;
    normalized_u = u;
    normalized_v = v;
    return true;
}

RayOutcome trace_one(const HeightField& plate, const PlatePose& pose,
                     const LightSource& source, const std::vector<RectReceiver>& receivers,
                     const TraceConfig& config, std::uint64_t ray_index,
                     double rotation_cos, double rotation_sin, const SplineBounds* bounds) {
    RayOutcome outcome;
    double sample_x = random01(config.seed, ray_index, 0);
    double sample_y = random01(config.seed, ray_index, 1);
    if (config.stratify_entrance) {
        const auto rows = static_cast<std::uint64_t>(std::sqrt(static_cast<double>(config.ray_count)));
        const auto short_row = config.ray_count / rows;
        const auto long_count = config.ray_count % rows;
        const auto split = long_count * (short_row + 1);
        const auto columns = short_row + (ray_index < split);
        const auto local = ray_index < split ? ray_index : ray_index - split;
        const auto row_start = ray_index - local % columns;
        sample_x = (local % columns + sample_x) / columns;
        sample_y = (row_start + sample_y * columns) / config.ray_count;
    }
    const Vec3 front{(sample_x-.5)*plate.width_mm, (sample_y-.5)*plate.height_mm, 0.0};
    const Vec3 front_world = rotate_y(front, rotation_cos, rotation_sin) + pose.translation_mm;
    Vec3 source_world = source.center;
    double source_area = 1.0;
    double cos_emission = 1.0;
    if (source.kind == LightSourceKind::Rectangle) {
        source_world += source.u * ((random01(config.seed, ray_index, 2) - 0.5) * source.width_mm);
        source_world += source.v * ((random01(config.seed, ray_index, 3) - 0.5) * source.height_mm);
        source_area = source.width_mm * source.height_mm;
        const Vec3 source_normal = cross(source.u, source.v);
        const Vec3 delta = front_world - source_world;
        const double distance = length(delta);
        if (!(distance > kEpsilon)) {
            outcome.kind = RayOutcome::Kind::SourceRejected;
            return outcome;
        }
        cos_emission = dot(source_normal, delta / distance);
        if (!(cos_emission > 0.0)) {
            outcome.kind = RayOutcome::Kind::SourceRejected;
            return outcome;
        }
    }
    Vec3 delta = front_world - source_world;
    double distance_squared = length_squared(delta);
    if (source.kind == LightSourceKind::SolarDisk) {
        // Apparent diameter at 1 AU: 1919 arcsec. Uniform radiance over the solar disc.
        constexpr double pi = 3.141592653589793;
        const double alpha = 1919.0*pi/(3600.0*180.0*2.0);
        const double radius = std::tan(alpha)*std::sqrt(random01(config.seed,ray_index,2));
        const double azimuth = 2*pi*random01(config.seed,ray_index,3);
        delta = cross(source.u,source.v) + source.u*(radius*std::cos(azimuth)) +
                source.v*(radius*std::sin(azimuth));
        distance_squared = 1.0; // Unit incident irradiance, independent of the marker position.
    }
    if (!(distance_squared > kEpsilon) || !std::isfinite(distance_squared)) {
        outcome.kind = RayOutcome::Kind::SourceRejected;
        return outcome;
    }
    const Vec3 incident = inverse_rotate_y(normalize(delta), rotation_cos, rotation_sin);
    const double cos_front = incident.z;
    if (!(cos_front > 0.0)) {
        outcome.kind = RayOutcome::Kind::SourceRejected;
        return outcome;
    }
    Vec3 inside;
    double front_transmission = 1.0;
    if (!refract(incident, {0.0, 0.0, -1.0}, 1.0, plate.refractive_index,
                 inside, front_transmission)) {
        outcome.kind = RayOutcome::Kind::NonFinite;
        return outcome;
    }
    ExitHit exit;
    const auto exit_status = bounds ? intersect_spline(plate, *bounds, front, inside, exit)
                                    : intersect_exit(plate, front, inside, exit);
    switch (exit_status) {
    case ExitStatus::SideEscape:
        outcome.kind = RayOutcome::Kind::SideEscape;
        return outcome;
    case ExitStatus::Invalid:
        outcome.kind = RayOutcome::Kind::NonFinite;
        return outcome;
    case ExitStatus::Hit:
        break;
    }
    const Vec3 back_normal = normalize({exit.surface.dx, exit.surface.dy, -1.0});
    Vec3 outgoing;
    double back_transmission = 1.0;
    if (!refract(inside, back_normal, plate.refractive_index, 1.0,
                 outgoing, back_transmission)) {
        outcome.kind = RayOutcome::Kind::InternalReflection;
        return outcome;
    }
    const double source_to_plate_flux = source_area * plate.width_mm * plate.height_mm *
                                        cos_emission * cos_front / distance_squared /
                                        static_cast<double>(config.ray_count);
    const double flux = source_to_plate_flux *
                        (config.fresnel ? front_transmission * back_transmission : 1.0);
    if (!(flux >= 0.0) || !std::isfinite(flux) || !finite(outgoing)) {
        outcome.kind = RayOutcome::Kind::NonFinite;
        return outcome;
    }
    const Vec3 exit_world = rotate_y(exit.position, rotation_cos, rotation_sin) +
                            pose.translation_mm;
    const Vec3 outgoing_world = rotate_y(outgoing, rotation_cos, rotation_sin);
    const Vec3 ray_origin = exit_world + outgoing_world * kSurfaceOffset;
    double nearest = std::numeric_limits<double>::infinity();
    std::size_t nearest_receiver = 0;
    std::size_t nearest_pixel = 0;
    for (std::size_t i = 0; i < receivers.size(); ++i) {
        double t = 0.0, u = 0.0, v = 0.0;
        if (!receiver_intersection(receivers[i], ray_origin, outgoing_world, t, u, v)) continue;
        if (t >= nearest) continue;
        nearest = t;
        nearest_receiver = i;
        const std::size_t pixel_x = std::min<std::size_t>(
            static_cast<std::size_t>((u + 0.5) * receivers[i].image_width_px),
            receivers[i].image_width_px - 1);
        const std::size_t pixel_y = std::min<std::size_t>(
            static_cast<std::size_t>((v + 0.5) * receivers[i].image_height_px),
            receivers[i].image_height_px - 1);
        nearest_pixel = pixel_y * receivers[i].image_width_px + pixel_x;
    }
    if (!std::isfinite(nearest)) {
        outcome.kind = RayOutcome::Kind::NoReceiver;
        return outcome;
    }
    outcome.kind = RayOutcome::Kind::ReceiverHit;
    outcome.receiver = nearest_receiver;
    outcome.pixel = nearest_pixel;
    const double pixel_area = receivers[nearest_receiver].width_mm *
                              receivers[nearest_receiver].height_mm /
                              (static_cast<double>(receivers[nearest_receiver].image_width_px) *
                               receivers[nearest_receiver].image_height_px);
    outcome.flux = flux / pixel_area;
    return outcome;
}

void validate_inputs(const HeightField& plate, const PlatePose& pose,
                     const LightSource& source, const std::vector<RectReceiver>& receivers,
                     const TraceConfig& config, std::size_t threads) {
    if (plate.samples_x < 2 || plate.samples_y < 2 ||
        plate.samples_x > 16384 || plate.samples_y > 16384 ||
        plate.offsets_mm.size() != static_cast<std::size_t>(plate.samples_x) * plate.samples_y ||
        !positive_finite(plate.width_mm) || !positive_finite(plate.height_mm) ||
        !positive_finite(plate.thickness_mm) ||
        !std::isfinite(plate.refractive_index) || plate.refractive_index <= 1.0)
        throw std::invalid_argument("invalid height field");
    for (double h : plate.offsets_mm) {
        if (!std::isfinite(h) || !(plate.thickness_mm + h > 0.0))
            throw std::invalid_argument("height field has non-finite or non-positive thickness");
    }
    if (!finite(pose.translation_mm) || !std::isfinite(pose.rotation_y_deg))
        throw std::invalid_argument("invalid plate pose");
    if (!finite(source.center) || !finite(source.u) || !finite(source.v))
        throw std::invalid_argument("invalid light source");
    if (source.kind != LightSourceKind::Point) {
        if ((source.kind==LightSourceKind::Rectangle && (!positive_finite(source.width_mm) || !positive_finite(source.height_mm))) ||
            std::abs(length(source.u) - 1.0) > 1e-6 ||
            std::abs(length(source.v) - 1.0) > 1e-6 ||
            std::abs(dot(source.u, source.v)) > 1e-6)
            throw std::invalid_argument("invalid rectangle light source");
    }
    if (receivers.empty()) throw std::invalid_argument("no receivers");
    for (const auto& receiver : receivers) {
        if (!finite(receiver.center) || !finite(receiver.u) || !finite(receiver.v) ||
            !positive_finite(receiver.width_mm) || !positive_finite(receiver.height_mm) ||
            std::abs(length(receiver.u) - 1.0) > 1e-6 ||
            std::abs(length(receiver.v) - 1.0) > 1e-6 ||
            std::abs(dot(receiver.u, receiver.v)) > 1e-6 ||
            receiver.image_width_px == 0 || receiver.image_height_px == 0)
            throw std::invalid_argument("invalid rectangular receiver");
    }
    if (config.ray_count == 0 || config.max_rays == 0 || config.ray_count > config.max_rays ||
        config.thread_count > 256 ||
        !positive_finite(config.max_seconds) || config.max_buffer_bytes == 0)
        throw std::invalid_argument("invalid tracing budget");
    const auto max = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t total_pixels = 0;
    for (const auto& receiver : receivers) {
        const std::uint64_t pixels = static_cast<std::uint64_t>(receiver.image_width_px) *
                                     receiver.image_height_px;
        if (pixels > max - total_pixels)
            throw std::invalid_argument("receiver image size overflows");
        total_pixels += pixels;
    }
    if (total_pixels > max / (threads + 1) / sizeof(double) ||
        total_pixels * (threads + 1) * sizeof(double) > config.max_buffer_bytes)
        throw std::invalid_argument("receiver image buffers exceed memory budget");
}

void count_outcome(TraceStats& stats, RayOutcome::Kind kind) {
    ++stats.processed;
    switch (kind) {
    case RayOutcome::Kind::SourceRejected: ++stats.source_rejected; break;
    case RayOutcome::Kind::SideEscape: ++stats.side_escape; break;
    case RayOutcome::Kind::InternalReflection: ++stats.total_internal_reflection; break;
    case RayOutcome::Kind::NoReceiver: ++stats.no_receiver; break;
    case RayOutcome::Kind::ReceiverHit: ++stats.receiver_hit; break;
    case RayOutcome::Kind::NonFinite: ++stats.non_finite; break;
    }
}

void add_stats(TraceStats& a, const TraceStats& b) {
    a.processed += b.processed;
    a.source_rejected += b.source_rejected;
    a.side_escape += b.side_escape;
    a.total_internal_reflection += b.total_internal_reflection;
    a.no_receiver += b.no_receiver;
    a.receiver_hit += b.receiver_hit;
    a.non_finite += b.non_finite;
}

} // namespace

TraceResult trace_caustic(const HeightField& plate, const PlatePose& pose,
                         const LightSource& source,
                         const std::vector<RectReceiver>& receivers,
                         const TraceConfig& config) {
    const auto started = std::chrono::steady_clock::now();
    const std::size_t requested_threads = config.thread_count != 0 ? config.thread_count :
                                          std::max(1u, std::thread::hardware_concurrency());
    const std::size_t threads = static_cast<std::size_t>(
        std::min<std::uint64_t>(requested_threads, std::max<std::uint64_t>(1, config.ray_count)));
    validate_inputs(plate, pose, source, receivers, config, threads);
    const bool smooth = plate.interpolation != HeightInterpolation::Bilinear;
    const SplineBounds bounds = smooth ? spline_bounds(plate) : SplineBounds{};

    TraceResult result;
    result.images.reserve(receivers.size());
    for (const auto& receiver : receivers) {
        const std::size_t pixels = static_cast<std::size_t>(receiver.image_width_px) *
                                   receiver.image_height_px;
        result.images.push_back({receiver.id, receiver.image_width_px, receiver.image_height_px,
                                 std::vector<double>(pixels, 0.0)});
    }
    std::vector<ThreadState> thread_states(threads);
    for (auto& state : thread_states) {
        state.images.resize(receivers.size());
        for (std::size_t r = 0; r < receivers.size(); ++r) {
            const std::size_t pixels = static_cast<std::size_t>(receivers[r].image_width_px) *
                                       receivers[r].image_height_px;
            state.images[r].assign(pixels, 0.0);
        }
    }
    const double radians = pose.rotation_y_deg * std::numbers::pi / 180.0;
    const double rotation_cos = std::cos(radians);
    const double rotation_sin = std::sin(radians);
    std::atomic_int stop{0}; // 0 running, 1 user cancel, 2 time budget.
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (std::size_t worker = 0; worker < threads; ++worker) {
        workers.emplace_back([&, worker] {
            const std::uint64_t quotient = config.ray_count / threads;
            const std::uint64_t remainder = config.ray_count % threads;
            const std::uint64_t begin = quotient * worker + std::min<std::uint64_t>(worker, remainder);
            const std::uint64_t end = begin + quotient + (worker < remainder ? 1 : 0);
            ThreadState& local = thread_states[worker];
            for (std::uint64_t i = begin; i < end; ++i) {
                if (((i - begin) & 255ULL) == 0) {
                    if (stop.load(std::memory_order_relaxed) != 0) break;
                    if (config.cancel_flag && config.cancel_flag->load(std::memory_order_relaxed)) {
                        int expected = 0;
                        stop.compare_exchange_strong(expected, 1, std::memory_order_relaxed);
                        break;
                    }
                    const double elapsed = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - started).count();
                    if (elapsed >= config.max_seconds) {
                        int expected = 0;
                        stop.compare_exchange_strong(expected, 2, std::memory_order_relaxed);
                        break;
                    }
                }
                const RayOutcome ray = trace_one(plate, pose, source, receivers, config, i,
                                                 rotation_cos, rotation_sin, smooth ? &bounds : nullptr);
                count_outcome(local.stats, ray.kind);
                if (ray.kind == RayOutcome::Kind::ReceiverHit)
                    local.images[ray.receiver][ray.pixel] += ray.flux;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    for (std::size_t i = 0; i < threads; ++i) {
        add_stats(result.stats, thread_states[i].stats);
        for (std::size_t r = 0; r < receivers.size(); ++r) {
            auto& output = result.images[r].values;
            const auto& local = thread_states[i].images[r];
            for (std::size_t p = 0; p < output.size(); ++p) output[p] += local[p];
        }
    }
    const int reason = stop.load(std::memory_order_relaxed);
    result.stop_reason = reason == 1 ? TraceStopReason::Cancelled :
                         reason == 2 ? TraceStopReason::TimedOut : TraceStopReason::Completed;
    result.elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    return result;
}

} // namespace caustic
