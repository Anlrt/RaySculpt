#include "optics.hpp"

#include <atomic>
#include <cmath>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using namespace caustic;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

HeightField flat_plate() {
    HeightField plate;
    plate.width_mm = 20;
    plate.height_mm = 20;
    plate.thickness_mm = 5;
    plate.offsets_mm.assign(4, 0.0);
    return plate;
}

RectReceiver screen(std::string id, double z_mm) {
    RectReceiver receiver;
    receiver.id = std::move(id);
    receiver.center = {0, 0, z_mm};
    receiver.width_mm = 50;
    receiver.height_mm = 50;
    receiver.image_width_px = 16;
    receiver.image_height_px = 16;
    return receiver;
}

TraceConfig config() {
    TraceConfig c;
    c.ray_count = 8'000;
    c.thread_count = 2;
    c.max_seconds = 10;
    return c;
}

void require_accounted(const TraceStats& s) {
    require(s.processed == s.source_rejected + s.side_escape +
                           s.total_internal_reflection + s.no_receiver +
                           s.receiver_hit + s.non_finite,
            "ray accounting mismatch");
}

void require_finite_nonnegative(const IrradianceImage& image) {
    require(image.values.size() == static_cast<std::size_t>(image.width) * image.height,
            "image dimensions mismatch");
    for (double value : image.values) {
        require(std::isfinite(value) && value >= 0.0, "invalid irradiance value");
    }
}

void test_flat_plate_point_source() {
    LightSource source;
    source.center = {0, 0, -100};
    const auto c = config();
    const auto result = trace_caustic(flat_plate(), {}, source, {screen("wall", 100)}, c);
    require(result.stop_reason == TraceStopReason::Completed, "point trace stopped early");
    require(result.stats.processed == c.ray_count, "point trace skipped rays");
    require(result.stats.receiver_hit > c.ray_count * 90 / 100,
            "flat plate missed too many wall rays");
    require(result.stats.total_internal_reflection == 0, "flat plate caused TIR");
    require_accounted(result.stats);
    require_finite_nonnegative(result.images[0]);
    const double total = std::accumulate(result.images[0].values.begin(),
                                         result.images[0].values.end(), 0.0);
    require(total > 0.0, "point source produced no irradiance");
    const auto repeat = trace_caustic(flat_plate(), {}, source, {screen("wall", 100)}, c);
    require(repeat.images[0].values == result.images[0].values,
            "same seed and thread count were not deterministic");
}

void test_area_source_and_nearest_screen() {
    LightSource source;
    source.kind = LightSourceKind::Rectangle;
    source.center = {0, 0, -100};
    source.width_mm = 4;
    source.height_mm = 4;
    const auto c = config();
    auto farther = screen("far", 100);
    auto nearer = screen("near", 50);
    const auto result = trace_caustic(flat_plate(), {}, source, {farther, nearer}, c);
    require(result.stats.receiver_hit > c.ray_count * 90 / 100,
            "area source missed too many screen rays");
    require_accounted(result.stats);
    require_finite_nonnegative(result.images[0]);
    require_finite_nonnegative(result.images[1]);
    const double near_total = std::accumulate(result.images[1].values.begin(),
                                              result.images[1].values.end(), 0.0);
    const double far_total = std::accumulate(result.images[0].values.begin(),
                                             result.images[0].values.end(), 0.0);
    require(near_total > 0.0 && far_total == 0.0, "nearest screen did not block farther screen");
}

void test_total_internal_reflection() {
    HeightField plate = flat_plate();
    plate.offsets_mm = {0.0, 30.0, 0.0, 30.0}; // Exit slope 1.5 > critical angle.
    LightSource source;
    source.center = {0, 0, -100'000};
    const auto c = config();
    const auto result = trace_caustic(plate, {}, source, {screen("wall", 100)}, c);
    require(result.stats.processed == c.ray_count, "TIR trace skipped rays");
    require(result.stats.total_internal_reflection > c.ray_count * 99 / 100,
            "TIR not detected");
    require(result.stats.non_finite == 0, "TIR generated non-finite rays");
    require_accounted(result.stats);
    require_finite_nonnegative(result.images[0]);
}

void test_rotation_and_cancel() {
    LightSource source;
    source.center = {0, 0, -100};
    PlatePose pose;
    pose.rotation_y_deg = 10;
    auto wide = screen("wide", 100);
    wide.width_mm = 100;
    wide.height_mm = 100;
    const auto c = config();
    const auto result = trace_caustic(flat_plate(), pose, source, {wide}, c);
    require(result.stats.processed == c.ray_count, "rotated trace skipped rays");
    require(result.stats.receiver_hit > 0, "rotated plate produced no screen hits");
    require_accounted(result.stats);

    std::atomic_bool cancelled{true};
    TraceConfig cancel_config = config();
    cancel_config.cancel_flag = &cancelled;
    const auto stopped = trace_caustic(flat_plate(), {}, source, {screen("wall", 100)},
                                      cancel_config);
    require(stopped.stop_reason == TraceStopReason::Cancelled, "cancel signal ignored");
    require(stopped.stats.processed == 0, "cancelled trace processed rays");
    require_accounted(stopped.stats);
}

void test_budget_validation() {
    LightSource source;
    source.center = {0, 0, -100};
    TraceConfig c = config();
    c.max_buffer_bytes = 1;
    bool rejected = false;
    try {
        (void)trace_caustic(flat_plate(), {}, source, {screen("wall", 100)}, c);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "undersized image budget was accepted");
}

} // namespace

int main() {
    try {
        test_flat_plate_point_source();
        test_area_source_and_nearest_screen();
        test_total_internal_reflection();
        test_rotation_and_cancel();
        test_budget_validation();
        std::cout << "optics tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "optics tests failed: " << error.what() << '\n';
        return 1;
    }
}
