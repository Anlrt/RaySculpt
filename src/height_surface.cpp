#include "height_surface.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace caustic {
namespace {

struct Basis {
    std::array<int, 4> index;
    std::array<double, 4> value;
    std::array<double, 4> derivative;
};

Basis basis(double x, double width, int count, bool linear_ends) {
    const double scale = (count - 1) / width;
    const double grid = std::clamp((x / width + 0.5) * (count - 1), 0.0,
                                   static_cast<double>(count - 1));
    const int i = std::min(static_cast<int>(grid), count - 2);
    const double t = grid - i;
    const double s = 1 - t;
    const double t2 = t * t;
    const double s2 = s * s;
    Basis b{{std::max(0, i - 1), i, i + 1, std::min(count - 1, i + 2)},
            {s2 * s / 6, (4 + 3 * t2 * (t - 2)) / 6,
             (4 + 3 * s2 * (s - 2)) / 6, t2 * t / 6},
            {-0.5 * s2 * scale, (1.5 * t2 - 2 * t) * scale,
             (2 * s - 1.5 * s2) * scale, 0.5 * t2 * scale}};
    if (linear_ends) {
        // Ghost coefficient c[-1] = 2*c[0]-c[1], likewise at the far end.
        for (auto* w : {&b.value, &b.derivative}) {
            if (i == 0) { (*w)[1] += (*w)[0]; (*w)[2] -= (*w)[0]; }
            if (i == count-2) { (*w)[2] += (*w)[3]; (*w)[1] -= (*w)[3]; }
        }
    }
    return b;
}

} // namespace

HeightSample sample_spline(const HeightField& plate, double x, double y) {
    // Uniform cardinal cubic, extended by repeating the end coefficients.
    // The same polynomials and derivatives are used by tracing and export.
    const bool linear = plate.interpolation == HeightInterpolation::CubicLinearEnds;
    const auto bx = basis(x, plate.width_mm, plate.samples_x, linear);
    const auto by = basis(y, plate.height_mm, plate.samples_y, linear);
    HeightSample result;
    for (int j = 0; j < 4; ++j) {
        double height = 0;
        double dx = 0;
        const auto row = static_cast<std::size_t>(by.index[j]) * plate.samples_x;
        for (int i = 0; i < 4; ++i) {
            const double z = plate.offsets_mm[row + bx.index[i]];
            height += z * bx.value[i];
            dx += z * bx.derivative[i];
        }
        result.height += height * by.value[j];
        result.dx += dx * by.value[j];
        result.dy += height * by.derivative[j];
    }
    return result;
}

SplineBounds spline_bounds(const HeightField& plate) {
    const int nx = plate.samples_x;
    const int ny = plate.samples_y;
    const double sx = (nx - 1) / plate.width_mm;
    const double sy = (ny - 1) / plate.height_mm;
    const auto [low, high] = std::minmax_element(plate.offsets_mm.begin(), plate.offsets_mm.end());
    SplineBounds b{plate.thickness_mm + *low, plate.thickness_mm + *high};
    const auto at = [&](int x, int y) {
        return plate.offsets_mm[static_cast<std::size_t>(y) * nx + x];
    };
    // A derivative of a cardinal spline is a convex combination of finite
    // differences of its coefficients in the respective dimension.
    for (int y = 0; y < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            const double z = at(x, y);
            const double forward_x = at(std::min(x + 1, nx - 1), y) - z;
            const double forward_y = at(x, std::min(y + 1, ny - 1)) - z;
            const double backward_x = z - at(std::max(x - 1, 0), y);
            const double backward_y = z - at(x, std::max(y - 1, 0));
            b.dx = std::max(b.dx, std::abs(forward_x) * sx);
            b.dy = std::max(b.dy, std::abs(forward_y) * sy);
            b.dxx = std::max(b.dxx, std::abs(forward_x - backward_x) * sx * sx);
            b.dyy = std::max(b.dyy, std::abs(forward_y - backward_y) * sy * sy);
            if (x + 1 < nx && y + 1 < ny) {
                const double difference = at(x + 1, y + 1) - at(x, y + 1) - forward_x;
                b.dxy = std::max(b.dxy, std::abs(difference) * sx * sy);
            }
        }
    }
    return b;
}

} // namespace caustic
