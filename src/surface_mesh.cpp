#include "surface_mesh.hpp"
#include "height_surface.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace caustic {
namespace {

void write_u32_le(std::ofstream& out, std::uint32_t value) {
    const std::array<char, 4> bytes{
        static_cast<char>(value), static_cast<char>(value >> 8),
        static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
    out.write(bytes.data(), bytes.size());
}

void write_float_le(std::ofstream& out, double value) {
    if (!std::isfinite(value) ||
        std::abs(value) > static_cast<double>(std::numeric_limits<float>::max()))
        throw std::invalid_argument("mesh coordinate exceeds STL float range");
    write_u32_le(out, std::bit_cast<std::uint32_t>(static_cast<float>(value)));
}

void write_vec3(std::ofstream& out, Vec3 value) {
    write_float_le(out, value.x);
    write_float_le(out, value.y);
    write_float_le(out, value.z);
}

} // namespace

SplineTessellation tessellate_to_tolerance(const HeightField& spline,
                                          double height_mm, double normal_degrees) {
    if (spline.interpolation == HeightInterpolation::Bilinear ||
        spline.samples_x < 2 || spline.samples_y < 2 ||
        spline.offsets_mm.size() != static_cast<std::size_t>(spline.samples_x) * spline.samples_y ||
        !(height_mm > 0) || !(normal_degrees > 0) ||
        !std::isfinite(height_mm) || !std::isfinite(normal_degrees))
        throw std::invalid_argument("invalid spline or STL tolerances");
    const auto b = spline_bounds(spline);
    const double hx = spline.width_mm / (spline.samples_x - 1);
    const double hy = spline.height_mm / (spline.samples_y - 1);
    // Compare the spline first to the rectangular bilinear interpolant, then
    // the bilinear surface to the two triangles sharing the cell diagonal.
    const double height = (b.dxx * hx * hx + b.dyy * hy * hy) / 8 + b.dxy * hx * hy / 4;
    // Facet gradients are secants along axis-aligned edges. Bound each
    // gradient component using the second derivatives over the full cell.
    const double normal = std::hypot(b.dxx * hx / 2 + b.dxy * hy,
                                     b.dxy * hx + b.dyy * hy / 2) * 180 / std::numbers::pi;
    const double subdivisions = std::ceil(std::max({1.0, std::sqrt(height / height_mm),
                                                    normal / normal_degrees}));
    constexpr unsigned max_cells = 1024;
    const unsigned spline_cells = std::max(spline.samples_x, spline.samples_y) - 1;
    if (!std::isfinite(subdivisions) || subdivisions > max_cells / spline_cells)
        throw std::invalid_argument("STL tolerances require more than 1024 cells per side");
    return {static_cast<unsigned>(subdivisions),
            height / (subdivisions * subdivisions), normal / subdivisions};
}

Mesh make_height_field_solid(const HeightField& plate, unsigned cell_subdivisions) {
    const std::size_t source_x = plate.samples_x;
    const std::size_t source_y = plate.samples_y;
    if (source_x < 2 || source_y < 2 || cell_subdivisions == 0 ||
        source_x - 1 > 2048 / cell_subdivisions || source_y - 1 > 2048 / cell_subdivisions ||
        !std::isfinite(plate.width_mm) || plate.width_mm <= 0.0 ||
        !std::isfinite(plate.height_mm) || plate.height_mm <= 0.0 ||
        !std::isfinite(plate.thickness_mm) || plate.thickness_mm <= 0.0 ||
        plate.offsets_mm.size() != source_x * source_y)
        throw std::invalid_argument("invalid height field for solid export");
    const std::size_t nx = (source_x - 1) * cell_subdivisions + 1;
    const std::size_t ny = (source_y - 1) * cell_subdivisions + 1;
    const std::size_t grid_size = nx * ny;
    const bool smooth = plate.interpolation != HeightInterpolation::Bilinear;
    Mesh mesh;
    mesh.vertices.reserve(smooth ? grid_size + 2 * (nx + ny - 2) + 1 : grid_size * 2);
    mesh.triangles.reserve(smooth ? 2 * (nx - 1) * (ny - 1) + 6 * (nx + ny - 2)
                                  : 4 * (nx - 1) * (ny - 1) + 4 * (nx + ny - 2));
    for (std::size_t y = 0; y < ny; ++y) {
        for (std::size_t x = 0; x < nx; ++x) {
            const double px = (static_cast<double>(x) / (nx - 1) - 0.5) * plate.width_mm;
            const double py = (static_cast<double>(y) / (ny - 1) - 0.5) * plate.height_mm;
            double offset = 0;
            if (smooth) {
                offset = sample_spline(plate, px, py).height;
            } else if (cell_subdivisions == 1) {
                offset = plate.offsets_mm[y * source_x + x];
            } else {
                const auto ix = std::min(x / cell_subdivisions, source_x - 2);
                const auto iy = std::min(y / cell_subdivisions, source_y - 2);
                const double u = static_cast<double>(x - ix * cell_subdivisions) / cell_subdivisions;
                const double v = static_cast<double>(y - iy * cell_subdivisions) / cell_subdivisions;
                const auto a = iy * source_x + ix;
                const auto& h = plate.offsets_mm;
                offset = std::lerp(std::lerp(h[a], h[a + 1], u),
                                   std::lerp(h[a + source_x], h[a + source_x + 1], u), v);
            }
            const double z = plate.thickness_mm + offset;
            if (!std::isfinite(z) || z <= 0.0)
                throw std::invalid_argument("exit surface must stay above the entrance");
            mesh.vertices.push_back({px, py, z});
        }
    }
    if (!smooth) {
        for (std::size_t i = 0; i < grid_size; ++i) {
            const auto p = mesh.vertices[i];
            mesh.vertices.push_back({p.x, p.y, 0.0});
        }
    }
    auto top = [nx](std::size_t x, std::size_t y) {
        return static_cast<std::uint32_t>(y * nx + x);
    };
    auto bottom = [grid_size, nx](std::size_t x, std::size_t y) {
        return static_cast<std::uint32_t>(grid_size + y * nx + x);
    };
    for (std::size_t y = 0; y + 1 < ny; ++y) {
        for (std::size_t x = 0; x + 1 < nx; ++x) {
            const auto a = top(x, y), b = top(x + 1, y);
            const auto c = top(x + 1, y + 1), d = top(x, y + 1);
            mesh.triangles.push_back({a, b, c});
            mesh.triangles.push_back({a, c, d});
            if (!smooth) {
                const auto e = bottom(x, y), f = bottom(x + 1, y);
                const auto g = bottom(x + 1, y + 1), h = bottom(x, y + 1);
                mesh.triangles.push_back({e, g, f});
                mesh.triangles.push_back({e, h, g});
            }
        }
    }
    std::vector<std::uint32_t> boundary;
    boundary.reserve(2 * (nx + ny - 2));
    for (std::size_t x = 0; x < nx; ++x) boundary.push_back(top(x, 0));
    for (std::size_t y = 1; y < ny; ++y) boundary.push_back(top(nx - 1, y));
    for (std::size_t x = nx - 1; x-- > 0;) boundary.push_back(top(x, ny - 1));
    for (std::size_t y = ny - 1; y-- > 1;) boundary.push_back(top(0, y));
    std::uint32_t bottom_center = 0;
    if (smooth) {
        for (const auto i : boundary) {
            const auto p = mesh.vertices[i];
            mesh.vertices.push_back({p.x, p.y, 0.0});
        }
        bottom_center = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({0, 0, 0});
    }
    for (std::size_t i = 0; i < boundary.size(); ++i) {
        const auto next = (i + 1) % boundary.size();
        const auto a = boundary[i];
        const auto b = boundary[next];
        const auto a0 = static_cast<std::uint32_t>(grid_size + (smooth ? i : a));
        const auto b0 = static_cast<std::uint32_t>(grid_size + (smooth ? next : b));
        mesh.triangles.push_back({a0, b0, b});
        mesh.triangles.push_back({a0, b, a});
        if (smooth) mesh.triangles.push_back({bottom_center, b0, a0});
    }
    std::string error;
    if (!validate_mesh(mesh, error))
        throw std::invalid_argument("height-field solid is invalid: " + error);
    return mesh;
}

bool save_binary_stl(const std::filesystem::path& path, const Mesh& mesh,
                     std::string& error) {
    if (!validate_mesh(mesh, error)) return false;
    if (mesh.triangles.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "STL triangle count exceeds 32-bit limit";
        return false;
    }
    try {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            error = "cannot open STL output";
            return false;
        }
        std::array<char, 80> header{};
        constexpr char label[] = "Caustic CAD; units: mm";
        std::memcpy(header.data(), label, sizeof(label) - 1);
        out.write(header.data(), header.size());
        write_u32_le(out, static_cast<std::uint32_t>(mesh.triangles.size()));
        for (const auto& t : mesh.triangles) {
            const Vec3 a = mesh.vertices[t.a];
            const Vec3 b = mesh.vertices[t.b];
            const Vec3 c = mesh.vertices[t.c];
            write_vec3(out, normalize(cross(b - a, c - a)));
            write_vec3(out, a);
            write_vec3(out, b);
            write_vec3(out, c);
            const std::array<char, 2> attributes{};
            out.write(attributes.data(), attributes.size());
        }
        out.close();
        if (!out) {
            error = "failed to write STL output";
            return false;
        }
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    error.clear();
    return true;
}

} // namespace caustic
