#pragma once

#include "optics.hpp"

namespace caustic {

struct HeightSample {
    double height{}; // Offset above nominal thickness, in mm.
    double dx{};
    double dy{};
};

struct SplineBounds {
    double min_z{};
    double max_z{};
    double dx{};
    double dy{};
    double dxx{};
    double dxy{};
    double dyy{};
};

// Inputs use a valid CubicSpline height field. X/Y are local coordinates.
HeightSample sample_spline(const HeightField& plate, double x, double y);
// Absolute derivative bounds over the entire surface.
SplineBounds spline_bounds(const HeightField& plate);

} // namespace caustic
