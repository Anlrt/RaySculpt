#include "optimizer.hpp"

#include <cmath>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>

using namespace caustic;

namespace {

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

RectReceiver receiver() {
    RectReceiver r;
    r.id = "screen";
    r.center = {0, 0, 100};
    r.width_mm = 60;
    r.height_mm = 60;
    r.image_width_px = 20;
    r.image_height_px = 20;
    return r;
}

void test_joint_inverse_problem() {
    HeightField truth;
    truth.samples_x = truth.samples_y = 3;
    truth.width_mm = truth.height_mm = 20;
    truth.thickness_mm = 5;
    truth.offsets_mm.assign(9, 0.0);
    truth.offsets_mm[4] = 0.6;
    HeightField initial = truth;
    initial.offsets_mm[4] = 0.0;
    LightSource source;
    source.center = {0, 0, -100};
    TraceConfig trace;
    trace.ray_count = 100'000;
    trace.thread_count = 2;
    trace.seed = 13;
    std::vector<OptimizationFrame> frames;
    for (double angle : {0.0, 12.0}) {
        OptimizationFrame frame;
        frame.pose.rotation_y_deg = angle;
        frame.receivers.push_back(receiver());
        const auto target = trace_caustic(truth, frame.pose, source,
                                          frame.receivers, trace);
        frame.targets.push_back(target.images.at(0).values);
        frames.push_back(std::move(frame));
    }
    OptimizationConfig config;
    config.trace = trace;
    config.max_evaluations = 40;
    config.max_seconds = 10;
    config.initial_step_mm = 0.2;
    config.min_step_mm = 0.025;
    config.editable_indices = {4};
    config.smoothness_weight = 0.0;
    const auto result = optimize_height_field(initial, source, frames, config);
    require(result.evaluations > 1, "optimizer did not evaluate candidates");
    require(result.final_loss < result.initial_loss * 0.2,
            "joint optimization failed to reduce both-frame error");
    require(std::abs(result.plate.offsets_mm[4] - 0.6) < 0.1,
            "optimizer did not recover the shared optic control point");

    // Use independent rays and an angle that was absent from training.
    TraceConfig validation = trace;
    validation.seed = 91;
    auto validation_error = [&](const HeightField& candidate) {
        double error = 0.0;
        for (double angle : {0.0, 6.0, 12.0}) {
            PlatePose pose;
            pose.rotation_y_deg = angle;
            const auto wanted = trace_caustic(truth, pose, source, {receiver()}, validation);
            const auto actual = trace_caustic(candidate, pose, source,
                                              {receiver()}, validation);
            const auto& a = actual.images[0].values;
            const auto& b = wanted.images[0].values;
            const double a_sum = std::accumulate(a.begin(), a.end(), 0.0);
            const double b_sum = std::accumulate(b.begin(), b.end(), 0.0);
            for (std::size_t i = 0; i < a.size(); ++i) {
                const double d = a[i] / a_sum - b[i] / b_sum;
                error += d * d;
            }
        }
        return error;
    };
    require(validation_error(result.plate) < validation_error(initial) * 0.3,
            "optimized plate failed validation on fresh rays/intermediate angle");

    config.max_evaluations = 2; // Flat baseline and the first positive trial.
    const auto bounded = optimize_height_field(initial, source, frames, config);
    require(bounded.stop_reason == OptimizationStopReason::Budget && bounded.evaluations == 2,
            "evaluation limit was not respected");
    require(bounded.final_loss < bounded.initial_loss && bounded.plate.offsets_mm[4] == 0.2,
            "evaluation limit discarded an already accepted improvement");
}

void test_invalid_target() {
    HeightField plate;
    LightSource source;
    source.center = {0, 0, -100};
    OptimizationFrame frame;
    frame.receivers.push_back(receiver());
    frame.targets.push_back({1.0});
    bool rejected = false;
    try {
        (void)optimize_height_field(plate, source, {frame}, {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "mismatched target size was accepted");

    frame.targets[0].assign(400, 1.0);
    OptimizationConfig config;
    config.max_target_pixels = 399;
    rejected = false;
    try {
        (void)optimize_height_field(plate, source, {frame}, config);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "pixel budget was ignored");
}

} // namespace

int main() {
    try {
        test_joint_inverse_problem();
        test_invalid_target();
        std::cout << "optimizer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "optimizer tests failed: " << error.what() << '\n';
        return 1;
    }
}
