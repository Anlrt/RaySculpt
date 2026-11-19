#include "geometry.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>
#include <sstream>
#include <stdexcept>

namespace caustic {

double length(Vec3 a) { return std::sqrt(length_squared(a)); }

Vec3 normalize(Vec3 a) {
    const double n = length(a);
    return n > 0.0 ? a / n : Vec3{};
}

bool finite(Vec3 a) {
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}

bool validate_mesh(const Mesh& mesh, std::string& error) {
    if (mesh.vertices.empty() || mesh.triangles.empty()) {
        error = "mesh has no vertices or triangles";
        return false;
    }
    if (mesh.vertices.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "mesh has too many vertices";
        return false;
    }
    for (const Vec3& vertex : mesh.vertices) {
        if (!finite(vertex)) {
            error = "mesh contains a non-finite vertex";
            return false;
        }
    }
    for (const Triangle& triangle : mesh.triangles) {
        if (triangle.a >= mesh.vertices.size() || triangle.b >= mesh.vertices.size() ||
            triangle.c >= mesh.vertices.size()) {
            error = "mesh contains an out-of-range triangle index";
            return false;
        }
        if (length_squared(cross(mesh.vertices[triangle.b] - mesh.vertices[triangle.a],
                                 mesh.vertices[triangle.c] - mesh.vertices[triangle.a])) < 1e-20) {
            error = "mesh contains a degenerate triangle";
            return false;
        }
    }
    error.clear();
    return true;
}

static bool parse_obj_index(const std::string& token, std::size_t count,
                            std::uint32_t& index, std::string& error) {
    const std::string vertex_token = token.substr(0, token.find('/'));
    if (vertex_token.empty()) {
        error = "face is missing a vertex index";
        return false;
    }
    long long value = 0;
    try {
        std::size_t used = 0;
        value = std::stoll(vertex_token, &used);
        if (used != vertex_token.size()) throw std::invalid_argument("trailing characters");
    } catch (const std::exception&) {
        error = "invalid OBJ vertex index";
        return false;
    }
    if (value == 0 || value > static_cast<long long>(count) ||
        value < -static_cast<long long>(count)) {
        error = "OBJ vertex index is out of range";
        return false;
    }
    index = static_cast<std::uint32_t>(value > 0 ? value - 1 : static_cast<long long>(count) + value);
    return true;
}

// A triangle fan can cover the empty corner of a concave OBJ face. Ear
// clipping preserves that corner and reports faces it cannot triangulate.
static bool triangulate_obj_face(const std::vector<std::uint32_t>& indices,
                                 const std::vector<Vec3>& vertices,
                                 std::vector<Triangle>& triangles,
                                 std::string& error) {
    if (indices.size() == 3) {
        triangles.push_back({indices[0], indices[1], indices[2]});
        return true;
    }
    if (indices.size() > 10000) {
        error = "OBJ face has too many vertices for safe triangulation";
        return false;
    }
    Vec3 normal{};
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const Vec3 a = vertices[indices[i]];
        const Vec3 b = vertices[indices[(i + 1) % indices.size()]];
        normal.x += (a.y - b.y) * (a.z + b.z);
        normal.y += (a.z - b.z) * (a.x + b.x);
        normal.z += (a.x - b.x) * (a.y + b.y);
    }
    const double ax = std::abs(normal.x);
    const double ay = std::abs(normal.y);
    const double az = std::abs(normal.z);
    const int dropped_axis = ax >= ay && ax >= az ? 0 : (ay >= az ? 1 : 2);
    struct Point2 { double x; double y; };
    std::vector<Point2> projected;
    projected.reserve(indices.size());
    for (const std::uint32_t index : indices) {
        const Vec3 p = vertices[index];
        if (dropped_axis == 0) projected.push_back({p.y, p.z});
        else if (dropped_axis == 1) projected.push_back({p.x, p.z});
        else projected.push_back({p.x, p.y});
    }
    const auto cross2 = [&](std::size_t a, std::size_t b, std::size_t c) {
        const Point2 p = projected[a];
        const Point2 q = projected[b];
        const Point2 r = projected[c];
        return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
    };
    double area2 = 0.0;
    double xmin = projected.front().x, xmax = xmin;
    double ymin = projected.front().y, ymax = ymin;
    for (std::size_t i = 0; i < projected.size(); ++i) {
        const Point2 a = projected[i];
        const Point2 b = projected[(i + 1) % projected.size()];
        area2 += a.x * b.y - a.y * b.x;
        xmin = std::min(xmin, a.x);
        xmax = std::max(xmax, a.x);
        ymin = std::min(ymin, a.y);
        ymax = std::max(ymax, a.y);
    }
    const double span = std::max(xmax - xmin, ymax - ymin);
    const double epsilon = 1e-12 * span * span;
    if (span <= 0.0 || std::abs(area2) <= epsilon) {
        error = "OBJ face has zero projected area";
        return false;
    }
    const double winding = area2 > 0.0 ? 1.0 : -1.0;
    std::vector<std::size_t> remaining(indices.size());
    for (std::size_t i = 0; i < remaining.size(); ++i) remaining[i] = i;
    while (remaining.size() > 3) {
        bool found_ear = false;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            const std::size_t prev = remaining[(i + remaining.size() - 1) % remaining.size()];
            const std::size_t mid = remaining[i];
            const std::size_t next = remaining[(i + 1) % remaining.size()];
            if (winding * cross2(prev, mid, next) <= epsilon) continue;
            bool contains_vertex = false;
            for (const std::size_t candidate : remaining) {
                if (candidate == prev || candidate == mid || candidate == next) continue;
                if (winding * cross2(prev, mid, candidate) >= -epsilon &&
                    winding * cross2(mid, next, candidate) >= -epsilon &&
                    winding * cross2(next, prev, candidate) >= -epsilon) {
                    contains_vertex = true;
                    break;
                }
            }
            if (contains_vertex) continue;
            triangles.push_back({indices[prev], indices[mid], indices[next]});
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
            found_ear = true;
            break;
        }
        if (!found_ear) {
            error = "OBJ face is self-intersecting or too degenerate to triangulate";
            return false;
        }
    }
    triangles.push_back({indices[remaining[0]], indices[remaining[1]], indices[remaining[2]]});
    return true;
}

bool load_obj(const std::filesystem::path& path, Mesh& mesh, std::string& error) {
    std::ifstream stream(path);
    if (!stream) {
        error = "cannot open OBJ file";
        return false;
    }
    Mesh loaded;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        std::istringstream row(line);
        std::string kind;
        row >> kind;
        if (kind == "v") {
            Vec3 v;
            if (!(row >> v.x >> v.y >> v.z) || !finite(v)) {
                error = "invalid vertex at line " + std::to_string(line_number);
                return false;
            }
            loaded.vertices.push_back(v);
        } else if (kind == "f") {
            std::vector<std::uint32_t> indices;
            std::string token;
            while (row >> token) {
                if (token[0] == '#') break;
                std::uint32_t index = 0;
                if (!parse_obj_index(token, loaded.vertices.size(), index, error)) {
                    error += " at line " + std::to_string(line_number);
                    return false;
                }
                indices.push_back(index);
            }
            if (indices.size() < 3) {
                error = "face has fewer than three vertices at line " + std::to_string(line_number);
                return false;
            }
            if (!triangulate_obj_face(indices, loaded.vertices, loaded.triangles, error)) {
                error += " at line " + std::to_string(line_number);
                return false;
            }
        }
    }
    if (stream.bad()) {
        error = "I/O error while reading OBJ file";
        return false;
    }
    if (!validate_mesh(loaded, error)) return false;
    mesh = std::move(loaded);
    return true;
}

Mesh make_cube(double edge_mm) {
    if (!(edge_mm > 0.0) || !std::isfinite(edge_mm)) throw std::invalid_argument("cube edge must be positive");
    const double h = edge_mm * 0.5;
    Mesh mesh;
    mesh.vertices = {
        {-h,-h,-h}, {h,-h,-h}, {h,h,-h}, {-h,h,-h},
        {-h,-h,h}, {h,-h,h}, {h,h,h}, {-h,h,h}
    };
    mesh.triangles = {
        {0,2,1}, {0,3,2}, {4,5,6}, {4,6,7},
        {0,1,5}, {0,5,4}, {1,2,6}, {1,6,5},
        {2,3,7}, {2,7,6}, {3,0,4}, {3,4,7}
    };
    return mesh;
}

Mesh make_uv_sphere(double radius_mm, int stacks, int slices) {
    if (!(radius_mm > 0.0) || !std::isfinite(radius_mm) || stacks < 3 || slices < 3)
        throw std::invalid_argument("invalid sphere dimensions or resolution");
    Mesh mesh;
    mesh.vertices.push_back({0, radius_mm, 0});
    for (int stack = 1; stack < stacks; ++stack) {
        const double phi = std::numbers::pi * stack / stacks;
        const double y = radius_mm * std::cos(phi);
        const double ring_radius = radius_mm * std::sin(phi);
        for (int slice = 0; slice < slices; ++slice) {
            const double theta = 2.0 * std::numbers::pi * slice / slices;
            mesh.vertices.push_back({ring_radius * std::cos(theta), y,
                                     ring_radius * std::sin(theta)});
        }
    }
    const std::uint32_t bottom = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back({0, -radius_mm, 0});
    for (int slice = 0; slice < slices; ++slice) {
        const std::uint32_t a = 1 + slice;
        const std::uint32_t b = 1 + (slice + 1) % slices;
        mesh.triangles.push_back({0, b, a});
    }
    for (int stack = 0; stack < stacks - 2; ++stack) {
        for (int slice = 0; slice < slices; ++slice) {
            const std::uint32_t a = 1 + stack * slices + slice;
            const std::uint32_t b = 1 + stack * slices + (slice + 1) % slices;
            const std::uint32_t c = a + slices;
            const std::uint32_t d = b + slices;
            mesh.triangles.push_back({a, b, c});
            mesh.triangles.push_back({b, d, c});
        }
    }
    const std::uint32_t last_ring = 1 + (stacks - 2) * slices;
    for (int slice = 0; slice < slices; ++slice) {
        mesh.triangles.push_back({bottom, last_ring + slice,
                                  last_ring + (slice + 1) % slices});
    }
    return mesh;
}

} // namespace caustic
