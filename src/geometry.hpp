#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace caustic {

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(double s, Vec3 a) { return a * s; }
inline Vec3 operator/(Vec3 a, double s) { return {a.x / s, a.y / s, a.z / s}; }
inline Vec3& operator+=(Vec3& a, Vec3 b) { a = a + b; return a; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length_squared(Vec3 a) { return dot(a, a); }
double length(Vec3 a);
Vec3 normalize(Vec3 a);
bool finite(Vec3 a);

struct Triangle {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
};

struct Mesh {
    std::vector<Vec3> vertices;
    std::vector<Triangle> triangles;
};

// OBJ vertex positions are interpreted as millimetres. The loader accepts
// polygonal faces and triangulates them while preserving their winding.
bool load_obj(const std::filesystem::path& path, Mesh& mesh, std::string& error);
Mesh make_cube(double edge_mm);
Mesh make_uv_sphere(double radius_mm, int stacks = 24, int slices = 48);
bool validate_mesh(const Mesh& mesh, std::string& error);

} // namespace caustic
