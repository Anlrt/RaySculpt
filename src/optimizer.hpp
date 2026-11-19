#pragma once

#include "optics.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace caustic {

// All frames share one physical height field. A frame may contain several
// receivers; targets[i] corresponds to receivers[i] in row-major order.
// Target brightness is comparable across frames (one exposure for all views).
struct OptimizationFrame {
    PlatePose pose;
    std::vector<RectReceiver> receivers;
    std::vector<std::vector<double>> targets;
    double weight = 1.0;
};

struct OptimizationConfig {
    TraceConfig trace;
    std::uint64_t max_evaluations = 500;
    std::size_t max_target_pixels = 1'000'000;
    double max_seconds = 30.0;
    double initial_step_mm = 0.25;
    double min_step_mm = 0.01;
    double max_abs_offset_mm = 2.0;
    double min_thickness_mm = 0.5;
    double smoothness_weight = 0.01;
    // Empty means all grid samples. Useful for coarse or masked optimization.
    std::vector<std::size_t> editable_indices;
    // An optional trial profile, accepted only if it improves the baseline.
    std::vector<double> starting_relief_mm;
    std::atomic_bool* cancel_flag = nullptr;
};

enum class OptimizationStopReason { Converged, Budget, TimedOut, Cancelled };

struct OptimizationResult {
    HeightField plate;
    double initial_loss = 0.0;
    double final_loss = 0.0;
    std::uint64_t evaluations = 0;
    OptimizationStopReason stop_reason = OptimizationStopReason::Converged;
};

// Deterministic coordinate search is a bounded baseline for small control
// grids. The same ray samples are used for every candidate (common random
// numbers), so candidate comparisons do not contain independent MC noise.
// The caller must validate the result with fresh rays and intermediate angles.
OptimizationResult optimize_height_field(const HeightField& initial,
                                         const LightSource& source,
                                         const std::vector<OptimizationFrame>& frames,
                                         const OptimizationConfig& config);

} // namespace caustic
