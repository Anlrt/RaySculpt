#pragma once

#include "geometry.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace caustic {

enum class LightSourceKind { Point, Rectangle, SolarDisk };

// All positions and lengths are in millimetres. For a Rectangle, u and v are
// unit, perpendicular directions and cross(u, v) is the emitting normal.
struct LightSource {
    LightSourceKind kind{LightSourceKind::Point};
    Vec3 center{};
    Vec3 u{1.0, 0.0, 0.0};
    Vec3 v{0.0, 1.0, 0.0};
    double width_mm{0.0};
    double height_mm{0.0};
};

// A finite rectangular projection surface. u and v must be unit and
// perpendicular; cross(u, v) is the front normal. Receivers are two-sided for
// geometric validation: the optical solver decides whether a side is usable.
struct RectReceiver {
    std::string id;
    Vec3 center{};
    Vec3 u{1.0, 0.0, 0.0};
    Vec3 v{0.0, 1.0, 0.0};
    double width_mm{0.0};
    double height_mm{0.0};
    std::uint32_t image_width_px{0};
    std::uint32_t image_height_px{0};
};

struct PreflightScene {
    Mesh optic;
    LightSource source;
    std::vector<RectReceiver> receivers;
    std::vector<double> angles_degrees{0.0};
    std::uint64_t rays_per_iteration{100'000};
    std::uint64_t iterations{100};
};

struct PreflightBudget {
    std::uint64_t max_primary_rays{500'000'000};
    std::uint64_t max_ray_screen_tests{2'000'000'000};
    std::uint64_t max_mesh_triangles{5'000'000};
};

enum class DiagnosticSeverity { Info, Warning, Error };
enum class PreflightMode { Interactive, Final };

struct Diagnostic {
    DiagnosticSeverity severity{DiagnosticSeverity::Info};
    std::string code;
    std::string object_id;
    std::string message;
    double angle_degrees{0.0};
    bool has_angle{false};
};

struct PreflightReport {
    std::vector<Diagnostic> diagnostics;
    std::uint64_t estimated_primary_rays{0};
    std::uint64_t estimated_ray_screen_tests{0};

    [[nodiscard]] bool can_start() const;
};

// Built once after each optic edit/import, not on every drag event. The caller
// must rebuild it whenever the mesh changes. Interactive preflight never
// recomputes these properties from the mesh.
struct PreflightMeshCache {
    bool valid{false};
    std::string error;
    Vec3 bounds_low{};
    Vec3 bounds_high{};
    bool closed_and_oriented{false};
    std::size_t vertex_count{0};
    std::size_t triangle_count{0};
};

[[nodiscard]] PreflightMeshCache build_preflight_cache(const Mesh& mesh);

// Only deterministic geometry/input faults produce Error. Sparse or approximate
// optical estimates must be Warning, never a declaration of impossibility.
[[nodiscard]] PreflightReport run_preflight(
    const PreflightScene& scene,
    const PreflightBudget& budget = {},
    PreflightMode mode = PreflightMode::Final,
    const PreflightMeshCache* cache = nullptr);

} // namespace caustic
