#include "target_renderer.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using caustic::GrayImage;
using caustic::Mesh;
using caustic::RenderSettings;
using caustic::Triangle;
using caustic::Vec3;

namespace {

void check(bool condition) {
    if (!condition) {
        throw std::runtime_error("target renderer check failed");
    }
}

void append_square(Mesh& mesh, double center_x, double center_z, double half_size) {
    const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back({center_x - half_size, -half_size, center_z});
    mesh.vertices.push_back({center_x + half_size, -half_size, center_z});
    mesh.vertices.push_back({center_x + half_size, half_size, center_z});
    mesh.vertices.push_back({center_x - half_size, half_size, center_z});
    mesh.triangles.push_back({first, first + 1, first + 2});
    mesh.triangles.push_back({first, first + 2, first + 3});
}

double mean_nonzero_half(const GrayImage& image, bool left) {
    double sum = 0.0;
    int count = 0;
    const int x_start = left ? 0 : image.width / 2;
    const int x_end = left ? image.width / 2 : image.width;
    for (int y = 0; y < image.height; ++y) {
        for (int x = x_start; x < x_end; ++x) {
            const auto value = image.pixels[static_cast<std::size_t>(y) * image.width + x];
            if (value > 0) {
                sum += value;
                ++count;
            }
        }
    }
    return count > 0 ? sum / count : 0.0;
}

void test_foreground_background_and_determinism() {
    const Mesh cube = caustic::make_cube(20.0);
    RenderSettings settings;
    settings.width = 128;
    settings.height = 128;
    settings.elevation_degrees = 20.0;
    const GrayImage first = caustic::render_target(cube, settings);
    const GrayImage second = caustic::render_target(cube, settings);
    check(first.width == 128 && first.height == 128);
    check(first.pixels.size() == 128u * 128u);
    check(first.pixels == second.pixels);
    check(first.pixels.front() == 0);
    check(first.pixels[64u * 128u + 64u] > 0);
}

void test_depth_cue() {
    Mesh mesh;
    append_square(mesh, -8.0, 5.0, 3.0);
    append_square(mesh, 8.0, -5.0, 3.0);
    RenderSettings settings;
    settings.width = 160;
    settings.height = 100;
    settings.elevation_degrees = 0.0;
    settings.depth_weight = 1.0;
    settings.ambient_weight = 0.0;
    settings.edge_weight = 0.0;
    const GrayImage image = caustic::render_target(mesh, settings);
    check(mean_nonzero_half(image, true) > mean_nonzero_half(image, false) + 25.0);
}

void test_hidden_geometry_does_not_change_image() {
    Mesh front;
    append_square(front, 0.0, 5.0, 7.0);
    // Retain both layers in the frame bounds for a fair comparison.
    front.vertices.push_back({0.0, 0.0, -5.0});
    Mesh layered = front;
    append_square(layered, 0.0, -5.0, 7.0);
    RenderSettings settings;
    settings.width = 96;
    settings.height = 96;
    settings.elevation_degrees = 0.0;
    settings.depth_weight = 0.0;
    settings.ambient_weight = 1.0;
    settings.edge_weight = 0.0;
    check(caustic::render_target(front, settings).pixels ==
           caustic::render_target(layered, settings).pixels);
}

void test_rotation_changes_asymmetric_view() {
    Mesh mesh = caustic::make_cube(20.0);
    Mesh second = caustic::make_cube(8.0);
    const auto offset = static_cast<std::uint32_t>(mesh.vertices.size());
    for (Vec3 point : second.vertices) {
        point.x += 14.0;
        point.z += 6.0;
        mesh.vertices.push_back(point);
    }
    for (Triangle face : second.triangles) {
        mesh.triangles.push_back({face.a + offset, face.b + offset, face.c + offset});
    }
    RenderSettings settings;
    settings.width = 128;
    settings.height = 128;
    settings.elevation_degrees = 10.0;
    const auto front = caustic::render_target(mesh, settings);
    settings.azimuth_degrees = 90.0;
    const auto side = caustic::render_target(mesh, settings);
    const auto changed = std::mismatch(front.pixels.begin(), front.pixels.end(), side.pixels.begin());
    check(changed.first != front.pixels.end());
}

void test_invalid_inputs_fail_before_allocating() {
    const Mesh cube = caustic::make_cube(10.0);
    RenderSettings settings;
    settings.width = 100000;
    settings.height = 100000;
    bool rejected = false;
    try {
        caustic::render_target(cube, settings);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected);

    settings.width = 64;
    settings.height = 64;
    settings.depth_weight = 0.9;
    settings.ambient_weight = 0.5;
    rejected = false;
    try {
        caustic::render_target(cube, settings);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected);
}

void test_parallel_rendering_is_deterministic() {
    const Mesh sphere = caustic::make_uv_sphere(12.0, 12, 24);
    RenderSettings settings;
    settings.width = 256;
    settings.height = 256;
    settings.azimuth_degrees = 37.0;
    settings.elevation_degrees = 12.0;
    const GrayImage first = caustic::render_target(sphere, settings);
    const GrayImage second = caustic::render_target(sphere, settings);
    check(first.pixels == second.pixels);
    check(first.pixels[128u * 256u + 128u] > 0);
}

} // namespace

int main() {
    test_foreground_background_and_determinism();
    test_depth_cue();
    test_hidden_geometry_does_not_change_image();
    test_rotation_changes_asymmetric_view();
    test_invalid_inputs_fail_before_allocating();
    test_parallel_rendering_is_deterministic();
    std::cout << "target renderer tests passed\n";
}
