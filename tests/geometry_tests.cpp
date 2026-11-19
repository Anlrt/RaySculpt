#include "geometry.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace caustic;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    std::string error;
    const Mesh cube = make_cube(20.0);
    require(validate_mesh(cube, error), "cube must be valid");
    require(cube.triangles.size() == 12, "cube must have 12 triangles");
    for (const Triangle& face : cube.triangles) {
        const Vec3 a = cube.vertices[face.a];
        const Vec3 b = cube.vertices[face.b];
        const Vec3 c = cube.vertices[face.c];
        require(dot(cross(b - a, c - a), (a + b + c) / 3.0) > 0.0,
                "cube normals must point outwards");
    }

    const Mesh sphere = make_uv_sphere(10.0, 8, 16);
    require(validate_mesh(sphere, error), "sphere must be valid");
    require(sphere.triangles.size() == 2u * 16u * 7u, "sphere triangle count");
    for (const Triangle& face : sphere.triangles) {
        const Vec3 a = sphere.vertices[face.a];
        const Vec3 b = sphere.vertices[face.b];
        const Vec3 c = sphere.vertices[face.c];
        require(dot(cross(b - a, c - a), (a + b + c) / 3.0) > 0.0,
                "sphere normals must point outwards");
    }

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "caustic_geometry_test.obj";
    {
        std::ofstream out(path);
        out << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n";
        out << "f -4 -3 -2 -1\n";
    }
    Mesh imported;
    require(load_obj(path, imported, error), "OBJ quad with negative indices must load");
    require(imported.triangles.size() == 2, "OBJ quad must triangulate");
    std::filesystem::remove(path);
    std::cout << "geometry tests passed\n";
}
