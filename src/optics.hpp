#pragma once

#include "preflight.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace caustic {

enum class HeightInterpolation { Bilinear, CubicSpline, CubicLinearEnds };

// A single-valued exit surface. The illuminated entrance is the plane z=0.
// The exit surface is z=thickness+offset(x,y) in millimetres, over the full
// rectangular aperture. For CubicSpline, offsets are spline coefficients.
struct HeightField {
    std::uint32_t samples_x = 2;
    std::uint32_t samples_y = 2;
    double width_mm = 100.0;
    double height_mm = 100.0;
    double thickness_mm = 5.0;
    double refractive_index = 1.49;
    std::vector<double> offsets_mm{0.0, 0.0, 0.0, 0.0};
    HeightInterpolation interpolation{HeightInterpolation::Bilinear};
};

struct PlatePose {
    Vec3 translation_mm{};
    double rotation_y_deg = 0.0;
};

struct TraceConfig {
    std::uint64_t ray_count = 100'000;
    std::uint64_t max_rays = 10'000'000;
    std::uint64_t seed = 1;
    std::uint32_t thread_count = 0; // 0 uses hardware_concurrency().
    double max_seconds = 30.0;
    std::uint64_t max_buffer_bytes = 256ULL * 1024ULL * 1024ULL;
    bool fresnel = true;
    std::atomic_bool* cancel_flag = nullptr;
    bool stratify_entrance = false;
    bool profile_timing = false;
};

struct TraceStats {
    std::uint64_t processed = 0;
    std::uint64_t source_rejected = 0;
    std::uint64_t side_escape = 0;
    std::uint64_t total_internal_reflection = 0;
    std::uint64_t no_receiver = 0;
    std::uint64_t receiver_hit = 0;
    std::uint64_t non_finite = 0;
};

enum class TraceStopReason {
    Completed,
    Cancelled,
    TimedOut,
};

struct IrradianceImage {
    std::string receiver_id;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Relative irradiance in row-major order, with row 0 at receiver -v.
    std::vector<double> values;
    std::uint64_t hits = 0;
};

struct TraceResult {
    std::vector<IrradianceImage> images;
    TraceStats stats;
    TraceStopReason stop_reason = TraceStopReason::Completed;
    double elapsed_seconds = 0.0;
    double reduction_seconds = 0.0;
    double sampled_intersection_ns = 0.0;
    double sampled_accumulation_ns = 0.0;
    std::uint64_t intersection_samples = 0;
    std::uint64_t accumulation_samples = 0;
};

// Reuse for sequential traces from one caller. Each call still owns its result.
class TraceWorkspace {
public:
    TraceWorkspace();
    ~TraceWorkspace();
private:
    friend TraceResult trace_caustic(const HeightField&, const PlatePose&, const LightSource&,
                                     const std::vector<RectReceiver>&, const TraceConfig&, TraceWorkspace*);
    struct State;
    std::unique_ptr<State> state_;
};

// Throws std::invalid_argument for malformed geometry or budget settings.
// The ray index determines all random samples, so changing thread_count does
// not change which rays are traced. Accumulation order can change last bits.
TraceResult trace_caustic(const HeightField& plate,
                         const PlatePose& pose,
                         const LightSource& source,
                         const std::vector<RectReceiver>& receivers,
                         const TraceConfig& config, TraceWorkspace* workspace = nullptr);

} // namespace caustic
