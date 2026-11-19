#include "optimizer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_set>

namespace caustic {
namespace {

double smoothness_penalty(const HeightField& plate) {
    double sum = 0.0;
    std::size_t edges = 0;
    for (std::size_t y = 0; y < plate.samples_y; ++y) {
        for (std::size_t x = 0; x < plate.samples_x; ++x) {
            const std::size_t i = y * plate.samples_x + x;
            if (x + 1 < plate.samples_x) {
                const double d = plate.offsets_mm[i] - plate.offsets_mm[i + 1];
                sum += d * d;
                ++edges;
            }
            if (y + 1 < plate.samples_y) {
                const double d = plate.offsets_mm[i] - plate.offsets_mm[i + plate.samples_x];
                sum += d * d;
                ++edges;
            }
        }
    }
    return edges ? sum / static_cast<double>(edges) : 0.0;
}

void validate(const HeightField& plate, const std::vector<OptimizationFrame>& frames,
              const OptimizationConfig& config) {
    if (plate.samples_x < 2 || plate.samples_y < 2 ||
        plate.offsets_mm.size() != static_cast<std::size_t>(plate.samples_x) * plate.samples_y ||
        !std::isfinite(plate.thickness_mm) || plate.thickness_mm <= 0.0)
        throw std::invalid_argument("invalid height field");
    if (frames.empty()) throw std::invalid_argument("at least one target frame is required");
    if (config.max_evaluations < 1 || config.max_target_pixels == 0 ||
        !std::isfinite(config.max_seconds) ||
        config.max_seconds <= 0.0 || !std::isfinite(config.initial_step_mm) ||
        !std::isfinite(config.min_step_mm) || config.min_step_mm <= 0.0 ||
        config.initial_step_mm < config.min_step_mm ||
        !std::isfinite(config.max_abs_offset_mm) || config.max_abs_offset_mm <= 0.0 ||
        !std::isfinite(config.min_thickness_mm) || config.min_thickness_mm <= 0.0 ||
        !std::isfinite(config.smoothness_weight) || config.smoothness_weight < 0.0 ||
        plate.thickness_mm - config.max_abs_offset_mm < config.min_thickness_mm)
        throw std::invalid_argument("invalid optimization budget or thickness bounds");
    for (double h : plate.offsets_mm) {
        if (!std::isfinite(h) || std::abs(h) > config.max_abs_offset_mm)
            throw std::invalid_argument("initial offset is outside the allowed range");
    }
    std::unordered_set<std::size_t> seen;
    for (std::size_t index : config.editable_indices) {
        if (index >= plate.offsets_mm.size() || !seen.insert(index).second)
            throw std::invalid_argument("editable index is invalid or repeated");
    }
    if (!config.starting_relief_mm.empty()) {
        if (config.starting_relief_mm.size() != plate.offsets_mm.size())
            throw std::invalid_argument("starting relief grid differs from the initial grid");
        for (std::size_t i = 0; i < config.starting_relief_mm.size(); ++i) {
            const double h = config.starting_relief_mm[i];
            if (!std::isfinite(h) || std::abs(h) > config.max_abs_offset_mm ||
                (!seen.empty() && !seen.contains(i) && h != plate.offsets_mm[i]))
                throw std::invalid_argument("starting relief violates optimization constraints");
        }
    }
    std::size_t total_target_pixels = 0;
    for (const auto& frame : frames) {
        if (frame.receivers.empty() || frame.receivers.size() != frame.targets.size() ||
            !std::isfinite(frame.weight) || frame.weight <= 0.0)
            throw std::invalid_argument("invalid optimization frame");
        for (std::size_t i = 0; i < frame.receivers.size(); ++i) {
            const auto& r = frame.receivers[i];
            const auto& target = frame.targets[i];
            if (r.image_width_px == 0 || r.image_height_px == 0 ||
                target.size() != static_cast<std::size_t>(r.image_width_px) * r.image_height_px)
                throw std::invalid_argument("target image size differs from receiver");
            if (target.size() > config.max_target_pixels - total_target_pixels)
                throw std::invalid_argument("target images exceed the pixel budget");
            total_target_pixels += target.size();
            double sum = 0.0;
            for (double value : target) {
                if (!std::isfinite(value) || value < 0.0)
                    throw std::invalid_argument("target contains an invalid intensity");
                sum += value;
            }
            if (!(sum > 0.0) || !std::isfinite(sum))
                throw std::invalid_argument("target must have positive finite brightness");
        }
    }
}

} // namespace

OptimizationResult optimize_height_field(const HeightField& initial,
                                         const LightSource& source,
                                         const std::vector<OptimizationFrame>& frames,
                                         const OptimizationConfig& config) {
    validate(initial, frames, config);
    OptimizationResult result;
    result.plate = initial;
    const auto start = std::chrono::steady_clock::now();
    auto remaining_seconds = [&]() {
        return config.max_seconds -
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    auto stopped = [&]() {
        if (config.cancel_flag && config.cancel_flag->load()) {
            result.stop_reason = OptimizationStopReason::Cancelled;
            return true;
        }
        if (remaining_seconds() <= 0.0) {
            result.stop_reason = OptimizationStopReason::TimedOut;
            return true;
        }
        if (result.evaluations >= config.max_evaluations) {
            result.stop_reason = OptimizationStopReason::Budget;
            return true;
        }
        return false;
    };
    auto evaluate = [&](const HeightField& plate) {
        double loss = config.smoothness_weight * smoothness_penalty(plate);
        struct Observation {
            std::vector<double> actual;
            const std::vector<double>* target;
            double weight;
        };
        std::vector<Observation> observations;
        double actual_sum = 0.0;
        double target_sum = 0.0;
        std::size_t pixel_count = 0;
        for (const auto& frame : frames) {
            if (config.cancel_flag && config.cancel_flag->load()) {
                result.stop_reason = OptimizationStopReason::Cancelled;
                return std::numeric_limits<double>::quiet_NaN();
            }
            const double remaining = remaining_seconds();
            if (remaining <= 0.0) {
                result.stop_reason = OptimizationStopReason::TimedOut;
                return std::numeric_limits<double>::quiet_NaN();
            }
            TraceConfig trace_config = config.trace;
            trace_config.max_seconds = std::min(trace_config.max_seconds, remaining);
            trace_config.cancel_flag = config.cancel_flag;
            auto traced = trace_caustic(plate, frame.pose, source,
                                        frame.receivers, trace_config);
            if (traced.stop_reason != TraceStopReason::Completed) {
                result.stop_reason = traced.stop_reason == TraceStopReason::Cancelled ?
                    OptimizationStopReason::Cancelled : OptimizationStopReason::TimedOut;
                return std::numeric_limits<double>::quiet_NaN();
            }
            for (std::size_t i = 0; i < frame.targets.size(); ++i) {
                auto& values = traced.images[i].values;
                actual_sum += frame.weight *
                    std::accumulate(values.begin(), values.end(), 0.0);
                target_sum += frame.weight *
                    std::accumulate(frame.targets[i].begin(), frame.targets[i].end(), 0.0);
                pixel_count += values.size();
                observations.push_back({std::move(values), &frame.targets[i], frame.weight});
            }
        }
        if (!(actual_sum > 0.0) || !std::isfinite(actual_sum))
            loss += static_cast<double>(pixel_count);
        else {
            double image_error = 0.0;
            for (const auto& observation : observations) {
                for (std::size_t i = 0; i < observation.actual.size(); ++i) {
                    const double d = observation.actual[i] / actual_sum -
                                     (*observation.target)[i] / target_sum;
                    image_error += observation.weight * d * d;
                }
            }
            loss += image_error * static_cast<double>(pixel_count);
        }
        ++result.evaluations;
        return loss;
    };

    if (stopped()) return result;
    result.initial_loss = evaluate(result.plate);
    result.final_loss = result.initial_loss;
    if (!std::isfinite(result.initial_loss)) return result;
    if (!config.starting_relief_mm.empty()) {
        if (stopped()) return result;
        HeightField trial = result.plate;
        trial.offsets_mm = config.starting_relief_mm;
        const double loss = evaluate(trial);
        if (!std::isfinite(loss)) return result;
        if (loss < result.final_loss) {
            result.plate = std::move(trial);
            result.final_loss = loss;
        }
    }
    std::vector<std::size_t> indices = config.editable_indices;
    if (indices.empty()) {
        indices.resize(result.plate.offsets_mm.size());
        std::iota(indices.begin(), indices.end(), 0);
    }
    double step = config.initial_step_mm;
    while (step >= config.min_step_mm) {
        bool improved = false;
        for (std::size_t index : indices) {
            if (stopped()) return result;
            const double original = result.plate.offsets_mm[index];
            double best_value = original;
            double best_loss = result.final_loss;
            for (int sign : {1, -1}) {
                const double candidate = original + sign * step;
                if (std::abs(candidate) > config.max_abs_offset_mm) continue;
                if (stopped()) break;
                result.plate.offsets_mm[index] = candidate;
                const double loss = evaluate(result.plate);
                result.plate.offsets_mm[index] = original;
                if (!std::isfinite(loss)) break;
                if (loss < best_loss) {
                    best_loss = loss;
                    best_value = candidate;
                }
            }
            result.plate.offsets_mm[index] = best_value;
            if (best_value != original) {
                improved = true;
                result.final_loss = best_loss;
            }
            if (result.stop_reason != OptimizationStopReason::Converged) return result;
        }
        if (!improved) step *= 0.5;
    }
    return result;
}

} // namespace caustic
