#include "surface_mesh.hpp"
#include "height_surface.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>

using namespace caustic;

namespace {

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void test_closed_plate_and_stl() {
    HeightField plate;
    plate.samples_x = 3;
    plate.samples_y = 3;
    plate.width_mm = 20;
    plate.height_mm = 10;
    plate.thickness_mm = 5;
    plate.offsets_mm.assign(9, 0.0);
    plate.offsets_mm[4] = 0.5;
    const Mesh mesh = make_height_field_solid(plate);
    require(mesh.vertices.size() == 18, "wrong vertex count");
    require(mesh.triangles.size() == 32, "wrong triangle count");
    struct EdgeUse { int count = 0; int winding = 0; };
    std::map<std::pair<std::uint32_t, std::uint32_t>, EdgeUse> edges;
    for (const auto& t : mesh.triangles) {
        for (auto [a, b] : {std::pair{t.a, t.b}, std::pair{t.b, t.c},
                            std::pair{t.c, t.a}}) {
            const int winding = a < b ? 1 : -1;
            if (a > b) std::swap(a, b);
            auto& use = edges[{a, b}];
            ++use.count;
            use.winding += winding;
        }
    }
    for (const auto& [edge, use] : edges) {
        (void)edge;
        require(use.count == 2 && use.winding == 0,
                "solid has an open or inconsistently oriented edge");
    }
    const auto path = std::filesystem::temp_directory_path() / "caustic_surface_mesh_test.stl";
    std::string error;
    require(save_binary_stl(path, mesh, error), "STL write failed: " + error);
    require(std::filesystem::file_size(path) == 84 + 50 * mesh.triangles.size(),
            "binary STL has wrong size");
    std::filesystem::remove(path);
}

void test_invalid_thickness() {
    HeightField plate;
    plate.offsets_mm = {0, 0, 0, -6};
    bool rejected = false;
    try {
        (void)make_height_field_solid(plate);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "inverted plate was accepted");
}

void test_bilinear_subdivision() {
    HeightField plate;
    plate.offsets_mm = {0, 0, 0, 2};
    const auto refined = make_height_field_solid(plate, 2);
    require(refined.vertices[4].z == 5.5, "exported midpoint did not preserve bilinear height");
    require(refined.vertices[8].z == 7.0, "subdivision changed the original grid heights");
}

void test_spline_continuity_and_tessellation() {
    HeightField plate;
    plate.interpolation = HeightInterpolation::CubicSpline;
    plate.samples_x = plate.samples_y = 4;
    plate.width_mm = 16;
    plate.height_mm = 10;
    plate.offsets_mm = {0, 1, 0, 0, 0, 2, -1, 1, 0, -1, 0, 0.5, 1, 1, -1, 0};
    const double knot = -plate.width_mm / 6;
    const auto left = sample_spline(plate, knot - 1e-7, 0.4);
    const auto right = sample_spline(plate, knot + 1e-7, 0.4);
    require(std::hypot(left.dx - right.dx, left.dy - right.dy) < 1e-6,
            "normal discontinuity at an interior spline knot");
    const auto settings = tessellate_to_tolerance(plate, 0.005, 1);
    require(settings.height_error_mm <= 0.005 && settings.normal_error_deg <= 1,
            "chosen subdivision exceeds requested tolerances");
    const auto mesh = make_height_field_solid(plate, settings.subdivisions);
    const auto cells = 3 * settings.subdivisions;
    for (std::size_t i = 0; i < 2 * cells * cells; ++i) {
        const auto& t = mesh.triangles[i];
        const auto a = mesh.vertices[t.a];
        const auto b = mesh.vertices[t.b];
        const auto c = mesh.vertices[t.c];
        const auto normal = normalize(cross(b - a, c - a));
        for (const auto p : {(a + b + c) / 3, (a + b) / 2, (a + c) / 2, (b + c) / 2}) {
            const auto s = sample_spline(plate, p.x, p.y);
            require(std::abs(p.z - plate.thickness_mm - s.height) <= settings.height_error_mm,
                    "triangle exceeds spline height error bound");
            const double angle = std::acos(std::clamp(dot(normal, normalize({-s.dx, -s.dy, 1})),
                                                       -1.0, 1.0)) * 180 / std::numbers::pi;
            require(angle <= settings.normal_error_deg, "triangle exceeds spline normal error bound");
        }
    }
}

void test_linear_spline_ends_reproduce_a_tilt() {
    HeightField p;
    p.samples_x = 7; p.samples_y = 5;
    p.width_mm = 20; p.height_mm = 14;
    p.offsets_mm.clear();
    for (unsigned y=0; y<p.samples_y; ++y)
        for (unsigned x=0; x<p.samples_x; ++x)
            p.offsets_mm.push_back(.03*(p.width_mm*x/(p.samples_x-1)-p.width_mm/2)
                                  -.04*(p.height_mm*y/(p.samples_y-1)-p.height_mm/2));
    p.interpolation = HeightInterpolation::CubicSpline;
    require(std::abs(sample_spline(p,-10,0).dx-.015)<1e-12,"legacy edge convention changed");
    p.interpolation = HeightInterpolation::CubicLinearEnds;
    for (double x : {-10.,-9.5,0.,8.5,10.})
        for (double y : {-7.,-6.5,1.,7.}) {
            const auto s = sample_spline(p,x,y);
            require(std::abs(s.height-(.03*x-.04*y))<1e-12,"boundary height changed");
            require(std::abs(s.dx-.03)<1e-12 && std::abs(s.dy+.04)<1e-12,"boundary steers edge rays");
        }
    const auto solid = make_height_field_solid(p,2);
    for (const auto& v : solid.vertices)
        require(v.z==0 || std::abs(v.z-p.thickness_mm-(.03*v.x-.04*v.y))<1e-12,
                "mesh and smooth surface disagree on a tilted plane");
}

} // namespace

int main() {
    try {
        test_closed_plate_and_stl();
        test_invalid_thickness();
        test_bilinear_subdivision();
        test_spline_continuity_and_tessellation();
        test_linear_spline_ends_reproduce_a_tilt();
        std::cout << "surface mesh tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "surface mesh tests failed: " << error.what() << '\n';
        return 1;
    }
}
