#include "target_renderer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace caustic {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEpsilon = 1e-12;
constexpr int kMaxPixels = 4 * 1024 * 1024;

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(Vec3 v, double scalar) { return {v.x * scalar, v.y * scalar, v.z * scalar}; }
double v_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 v_cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
double v_length(Vec3 v) { return std::sqrt(v_dot(v, v)); }
Vec3 normalized(Vec3 v) {
    const double magnitude = v_length(v);
    return magnitude > kEpsilon ? mul(v, 1.0 / magnitude) : Vec3{0.0, 0.0, 0.0};
}
double clamp01(double value) { return std::clamp(value, 0.0, 1.0); }
double edge(double ax, double ay, double bx, double by, double px, double py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

struct ProjectedVertex {
    double x{};
    double y{};
    double z{};
};

struct PreparedTriangle {
    std::array<ProjectedVertex, 3> vertex;
    std::array<Vec3, 3> normal;
    double area{};
    int min_x{};
    int max_x{};
    int min_y{};
    int max_y{};
};

struct GeometryData {
    Vec3 center{};
    double radius{};
    std::vector<Vec3> face_normals;
    std::vector<std::array<Vec3, 3>> corner_normals;
};

GeometryData prepare_geometry(const Mesh& mesh) {
    if (mesh.vertices.empty() || mesh.triangles.empty()) {
        throw std::invalid_argument("target renderer requires a non-empty mesh");
    }

    Vec3 low = mesh.vertices.front();
    Vec3 high = low;
    for (const Vec3 vertex : mesh.vertices) {
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
            throw std::invalid_argument("mesh contains a non-finite coordinate");
        }
        low.x = std::min(low.x, vertex.x);
        low.y = std::min(low.y, vertex.y);
        low.z = std::min(low.z, vertex.z);
        high.x = std::max(high.x, vertex.x);
        high.y = std::max(high.y, vertex.y);
        high.z = std::max(high.z, vertex.z);
    }

    GeometryData data;
    data.center = mul(add(low, high), 0.5);
    for (const Vec3 vertex : mesh.vertices) {
        data.radius = std::max(data.radius, v_length(sub(vertex, data.center)));
    }
    if (!(data.radius > kEpsilon) || !std::isfinite(data.radius)) {
        throw std::invalid_argument("mesh has no usable extent");
    }

    data.face_normals.resize(mesh.triangles.size());
    data.corner_normals.resize(mesh.triangles.size());
    std::vector<std::vector<std::size_t>> incident(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
        const Triangle triangle = mesh.triangles[i];
        if (triangle.a >= mesh.vertices.size() || triangle.b >= mesh.vertices.size() ||
            triangle.c >= mesh.vertices.size()) {
            throw std::invalid_argument("triangle refers to a missing vertex");
        }
        const Vec3 a = mesh.vertices[triangle.a];
        const Vec3 b = mesh.vertices[triangle.b];
        const Vec3 c = mesh.vertices[triangle.c];
        data.face_normals[i] = v_cross(sub(b, a), sub(c, a));
        incident[triangle.a].push_back(i);
        incident[triangle.b].push_back(i);
        incident[triangle.c].push_back(i);
    }

    // Smooth curved regions while retaining creases above roughly 45 degrees.
    constexpr double smooth_cosine = 0.7071067811865476;
    for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
        const Triangle triangle = mesh.triangles[i];
        const std::array<std::uint32_t, 3> corners{triangle.a, triangle.b, triangle.c};
        const Vec3 current = normalized(data.face_normals[i]);
        for (int corner = 0; corner < 3; ++corner) {
            Vec3 sum{};
            for (const std::size_t neighbor : incident[corners[corner]]) {
                const Vec3 other = normalized(data.face_normals[neighbor]);
                if (v_dot(current, other) >= smooth_cosine) {
                    sum = add(sum, data.face_normals[neighbor]);
                }
            }
            data.corner_normals[i][corner] = v_length(sum) > kEpsilon ? normalized(sum) : current;
        }
    }
    return data;
}

double edge_strength(int x, int y, int width, int height,
                     const std::vector<double>& depth,
                     const std::vector<Vec3>& normal, double radius) {
    const std::size_t index = static_cast<std::size_t>(y) * width + x;
    double result = 0.0;
    constexpr std::array<std::array<int, 2>, 4> offsets{{{{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}}}};
    for (const auto& offset : offsets) {
        const int nx = x + offset[0];
        const int ny = y + offset[1];
        if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
            continue;
        }
        const std::size_t neighbor = static_cast<std::size_t>(ny) * width + nx;
        if (!std::isfinite(depth[neighbor])) {
            return 1.0;
        }
        const double orientation = clamp01((1.0 - v_dot(normal[index], normal[neighbor])) / 0.20);
        const double discontinuity =
            clamp01((std::abs(depth[index] - depth[neighbor]) / (2.0 * radius) - 0.012) / 0.035);
        result = std::max(result, std::max(orientation, discontinuity));
    }
    return result;
}

} // namespace

GrayImage render_target(const Mesh& mesh, const RenderSettings& settings) {
    const std::int64_t pixel_count = static_cast<std::int64_t>(settings.width) * settings.height;
    if (settings.width <= 0 || settings.height <= 0 || pixel_count > kMaxPixels) {
        throw std::invalid_argument("target dimensions must be positive and at most 4M pixels");
    }
    if (!std::isfinite(settings.azimuth_degrees) || !std::isfinite(settings.elevation_degrees) ||
        std::abs(settings.elevation_degrees) >= 89.5 ||
        !std::isfinite(settings.depth_weight) || !std::isfinite(settings.ambient_weight) ||
        !std::isfinite(settings.edge_weight) ||
        settings.depth_weight < 0.0 || settings.depth_weight > 1.0 ||
        settings.ambient_weight < 0.0 || settings.ambient_weight > 1.0 ||
        settings.edge_weight < 0.0 || settings.edge_weight > 1.0 ||
        settings.depth_weight + settings.ambient_weight > 1.0) {
        throw std::invalid_argument("invalid target rendering settings");
    }
    const GeometryData geometry = prepare_geometry(mesh);
    const int scale = pixel_count <= 1024 * 1024 ? 2 : 1;
    const int width = settings.width * scale;
    const int height = settings.height * scale;
    const std::size_t highres_pixels = static_cast<std::size_t>(width) * height;
    const double azimuth = settings.azimuth_degrees * kPi / 180.0;
    const double elevation = settings.elevation_degrees * kPi / 180.0;
    const Vec3 toward_camera{std::sin(azimuth) * std::cos(elevation),
                             std::sin(elevation),
                             std::cos(azimuth) * std::cos(elevation)};
    const Vec3 camera_forward = mul(toward_camera, -1.0);
    const Vec3 right = normalized(v_cross(camera_forward, {0.0, 1.0, 0.0}));
    const Vec3 up = v_cross(right, camera_forward);
    const Vec3 key_light = normalized(add(add(mul(right, 0.35), mul(up, 0.60)),
                                          mul(toward_camera, 0.72)));
    const Vec3 fill_light = normalized(add(add(mul(right, -0.60), mul(up, 0.28)),
                                           mul(toward_camera, 0.75)));
    const double aspect = static_cast<double>(width) / height;
    const double half_width = geometry.radius * 1.10 * std::max(1.0, aspect);
    const double half_height = geometry.radius * 1.10 * std::max(1.0, 1.0 / aspect);

    std::vector<ProjectedVertex> projected(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const Vec3 relative = sub(mesh.vertices[i], geometry.center);
        projected[i] = {(v_dot(relative, right) / (2.0 * half_width) + 0.5) * width,
                        (0.5 - v_dot(relative, up) / (2.0 * half_height)) * height,
                        v_dot(relative, toward_camera)};
    }

    std::vector<PreparedTriangle> triangles;
    triangles.reserve(mesh.triangles.size());
    constexpr int band_height = 64;
    const int band_count = (height + band_height - 1) / band_height;
    std::vector<std::vector<std::size_t>> bands(static_cast<std::size_t>(band_count));
    for (std::size_t i = 0; i < mesh.triangles.size(); ++i) {
        const Triangle indices = mesh.triangles[i];
        PreparedTriangle triangle;
        triangle.vertex = {projected[indices.a], projected[indices.b], projected[indices.c]};
        triangle.normal = geometry.corner_normals[i];
        triangle.area = edge(triangle.vertex[0].x, triangle.vertex[0].y,
                             triangle.vertex[1].x, triangle.vertex[1].y,
                             triangle.vertex[2].x, triangle.vertex[2].y);
        if (std::abs(triangle.area) < kEpsilon) {
            continue;
        }
        const double min_x = std::min({triangle.vertex[0].x, triangle.vertex[1].x, triangle.vertex[2].x});
        const double max_x = std::max({triangle.vertex[0].x, triangle.vertex[1].x, triangle.vertex[2].x});
        const double min_y = std::min({triangle.vertex[0].y, triangle.vertex[1].y, triangle.vertex[2].y});
        const double max_y = std::max({triangle.vertex[0].y, triangle.vertex[1].y, triangle.vertex[2].y});
        triangle.min_x = std::max(0, static_cast<int>(std::floor(min_x)));
        triangle.max_x = std::min(width - 1, static_cast<int>(std::ceil(max_x)));
        triangle.min_y = std::max(0, static_cast<int>(std::floor(min_y)));
        triangle.max_y = std::min(height - 1, static_cast<int>(std::ceil(max_y)));
        if (triangle.max_x < triangle.min_x || triangle.max_y < triangle.min_y) {
            continue;
        }
        const std::size_t prepared_index = triangles.size();
        triangles.push_back(triangle);
        for (int band = triangle.min_y / band_height; band <= triangle.max_y / band_height; ++band) {
            bands[band].push_back(prepared_index);
        }
    }

    std::vector<double> depth(highres_pixels, -std::numeric_limits<double>::infinity());
    std::vector<Vec3> normal(highres_pixels);
    const auto raster_band = [&](int band) {
        const int row_begin = band * band_height;
        const int row_end = std::min(height, row_begin + band_height);
        for (const std::size_t index : bands[band]) {
            const PreparedTriangle& triangle = triangles[index];
            for (int y = std::max(row_begin, triangle.min_y); y <= std::min(row_end - 1, triangle.max_y); ++y) {
                const double py = y + 0.5;
                for (int x = triangle.min_x; x <= triangle.max_x; ++x) {
                    const double px = x + 0.5;
                    const auto& a = triangle.vertex[0];
                    const auto& b = triangle.vertex[1];
                    const auto& c = triangle.vertex[2];
                    const double wa = edge(b.x, b.y, c.x, c.y, px, py) / triangle.area;
                    const double wb = edge(c.x, c.y, a.x, a.y, px, py) / triangle.area;
                    const double wc = 1.0 - wa - wb;
                    if (wa < -1e-10 || wb < -1e-10 || wc < -1e-10) {
                        continue;
                    }
                    const double z = wa * a.z + wb * b.z + wc * c.z;
                    const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
                    if (z > depth[pixel] + 1e-10) {
                        depth[pixel] = z;
                        normal[pixel] = normalized(add(add(mul(triangle.normal[0], wa),
                                                           mul(triangle.normal[1], wb)),
                                                       mul(triangle.normal[2], wc)));
                    }
                }
            }
        }
    };

    std::atomic<int> next_band{0};
    const auto worker = [&] {
        while (true) {
            const int band = next_band.fetch_add(1, std::memory_order_relaxed);
            if (band >= band_count) {
                return;
            }
            raster_band(band);
        }
    };
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const unsigned workers = highres_pixels >= 200000 && triangles.size() >= 64
                                 ? std::min<unsigned>(hardware, static_cast<unsigned>(band_count))
                                 : 1u;
    std::vector<std::thread> threads;
    threads.reserve(workers > 0 ? workers - 1 : 0);
    for (unsigned i = 1; i < workers; ++i) {
        threads.emplace_back(worker);
    }
    worker();
    for (auto& thread : threads) {
        thread.join();
    }

    const double ambient = clamp01(settings.ambient_weight);
    const double depth_weight = clamp01(settings.depth_weight);
    const double edge_weight = clamp01(settings.edge_weight);
    const double diffuse_weight = std::max(0.0, 1.0 - ambient - depth_weight);
    std::vector<double> luminance(highres_pixels, 0.0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            if (!std::isfinite(depth[index])) {
                continue;
            }
            Vec3 surface_normal = normal[index];
            if (v_dot(surface_normal, toward_camera) < 0.0) {
                surface_normal = mul(surface_normal, -1.0);
            }
            const double diffuse = 0.80 * std::max(0.0, v_dot(surface_normal, key_light)) +
                                   0.20 * std::max(0.0, v_dot(surface_normal, fill_light));
            const double near_value = clamp01((depth[index] + geometry.radius) / (2.0 * geometry.radius));
            const double border = edge_weight > 0.0
                                      ? edge_strength(x, y, width, height, depth, normal, geometry.radius)
                                      : 0.0;
            luminance[index] = clamp01(ambient + diffuse_weight * diffuse +
                                       depth_weight * near_value + edge_weight * border);
        }
    }

    GrayImage output;
    output.width = settings.width;
    output.height = settings.height;
    output.pixels.resize(static_cast<std::size_t>(pixel_count));
    for (int y = 0; y < settings.height; ++y) {
        for (int x = 0; x < settings.width; ++x) {
            double sum = 0.0;
            for (int sy = 0; sy < scale; ++sy) {
                for (int sx = 0; sx < scale; ++sx) {
                    sum += luminance[static_cast<std::size_t>(y * scale + sy) * width + x * scale + sx];
                }
            }
            const double value = sum / (scale * scale);
            output.pixels[static_cast<std::size_t>(y) * settings.width + x] =
                static_cast<std::uint8_t>(std::lround(255.0 * clamp01(value)));
        }
    }
    return output;
}

} // namespace caustic
