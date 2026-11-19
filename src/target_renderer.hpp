#pragma once

#include "geometry.hpp"

#include <cstdint>
#include <vector>

namespace caustic {

struct RenderSettings {
    int width{512};
    int height{512};
    double azimuth_degrees{0.0};
    double elevation_degrees{15.0};
    double depth_weight{0.15};
    double ambient_weight{0.20};
    double edge_weight{0.12};
};

struct GrayImage {
    int width{};
    int height{};
    std::vector<std::uint8_t> pixels;
};

// Orthographic view on a fixed, model-wide frame. Zero denotes no light.
// The same depth scale and exposure are used for every viewing angle.
GrayImage render_target(const Mesh& mesh, const RenderSettings& settings);

} // namespace caustic
