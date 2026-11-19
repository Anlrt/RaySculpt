#include "preflight.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace caustic {
namespace {

constexpr double kBasisTolerance = 1e-5;

bool positive_finite(double value) {
    return std::isfinite(value) && value > 0.0;
}

bool valid_basis(Vec3 u, Vec3 v) {
    return finite(u) && finite(v) &&
           std::abs(length_squared(u) - 1.0) <= kBasisTolerance &&
           std::abs(length_squared(v) - 1.0) <= kBasisTolerance &&
           std::abs(dot(u, v)) <= kBasisTolerance;
}

void add(PreflightReport& report, DiagnosticSeverity severity,
         std::string code, std::string object_id, std::string message) {
    report.diagnostics.push_back(
        {severity, std::move(code), std::move(object_id), std::move(message), 0.0, false});
}

bool checked_product(std::uint64_t a, std::uint64_t b, std::uint64_t& result) {
    if (b != 0 && a > std::numeric_limits<std::uint64_t>::max() / b) return false;
    result = a * b;
    return true;
}

struct Rectangle {
    Vec3 center;
    Vec3 u;
    Vec3 v;
    Vec3 normal;
    double half_width;
    double half_height;
};

Rectangle make_rectangle(Vec3 center, Vec3 u, Vec3 v, double width, double height) {
    return {center, u, v, normalize(cross(u, v)), width * 0.5, height * 0.5};
}

std::array<Vec3, 4> corners(const Rectangle& r) {
    const Vec3 a = r.u * r.half_width;
    const Vec3 b = r.v * r.half_height;
    return {r.center - a - b, r.center + a - b,
            r.center + a + b, r.center - a + b};
}

bool point_in_rectangle(Vec3 p, const Rectangle& r, double tolerance) {
    const Vec3 q = p - r.center;
    return std::abs(dot(q, r.normal)) <= tolerance &&
           std::abs(dot(q, r.u)) <= r.half_width + tolerance &&
           std::abs(dot(q, r.v)) <= r.half_height + tolerance;
}

bool overlapping_projection(const Rectangle& a, const Rectangle& b,
                            Vec3 axis, double tolerance) {
    const auto ca = corners(a);
    const auto cb = corners(b);
    double a_min = dot(axis, ca[0]);
    double a_max = a_min;
    double b_min = dot(axis, cb[0]);
    double b_max = b_min;
    for (Vec3 p : ca) {
        a_min = std::min(a_min, dot(axis, p));
        a_max = std::max(a_max, dot(axis, p));
    }
    for (Vec3 p : cb) {
        b_min = std::min(b_min, dot(axis, p));
        b_max = std::max(b_max, dot(axis, p));
    }
    return a_max + tolerance >= b_min && b_max + tolerance >= a_min;
}

bool segment_on_rectangle(Vec3 p0, Vec3 p1, const Rectangle& r, double tolerance) {
    const Vec3 o = p0 - r.center;
    const Vec3 d = p1 - p0;
    const std::array<double, 2> start{dot(o, r.u), dot(o, r.v)};
    const std::array<double, 2> delta{dot(d, r.u), dot(d, r.v)};
    const std::array<double, 2> half{r.half_width + tolerance, r.half_height + tolerance};
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 2; ++i) {
        if (std::abs(delta[i]) <= 1e-14) {
            if (std::abs(start[i]) > half[i]) return false;
        } else {
            double t0 = (-half[i] - start[i]) / delta[i];
            double t1 = (half[i] - start[i]) / delta[i];
            if (t0 > t1) std::swap(t0, t1);
            lo = std::max(lo, t0);
            hi = std::min(hi, t1);
            if (lo > hi) return false;
        }
    }
    return true;
}

bool segment_intersects_rectangle(Vec3 p0, Vec3 p1,
                                  const Rectangle& r, double tolerance) {
    const double d0 = dot(p0 - r.center, r.normal);
    const double d1 = dot(p1 - r.center, r.normal);
    if (std::abs(d0) <= tolerance && std::abs(d1) <= tolerance) {
        return segment_on_rectangle(p0, p1, r, tolerance);
    }
    if ((d0 > tolerance && d1 > tolerance) ||
        (d0 < -tolerance && d1 < -tolerance)) return false;
    const double denominator = d0 - d1;
    if (std::abs(denominator) <= 1e-14) return false;
    const double t = d0 / denominator;
    if (t < -1e-10 || t > 1.0 + 1e-10) return false;
    return point_in_rectangle(p0 + (p1 - p0) * t, r, tolerance);
}

bool rectangles_intersect(const Rectangle& a, const Rectangle& b, double tolerance) {
    // Coplanar rectangles are handled by four separating axes. For intersecting
    // planes, the common segment must meet an edge of at least one rectangle.
    if (length_squared(cross(a.normal, b.normal)) <= 1e-12) {
        if (std::abs(dot(b.center - a.center, a.normal)) > tolerance) return false;
        return overlapping_projection(a, b, a.u, tolerance) &&
               overlapping_projection(a, b, a.v, tolerance) &&
               overlapping_projection(a, b, b.u, tolerance) &&
               overlapping_projection(a, b, b.v, tolerance);
    }
    const auto ca = corners(a);
    const auto cb = corners(b);
    for (int i = 0; i < 4; ++i) {
        if (segment_intersects_rectangle(ca[i], ca[(i + 1) % 4], b, tolerance) ||
            segment_intersects_rectangle(cb[i], cb[(i + 1) % 4], a, tolerance))
            return true;
    }
    return false;
}

struct Bounds {
    Vec3 low;
    Vec3 high;
};

Bounds mesh_bounds(const Mesh& mesh) {
    Bounds bounds{mesh.vertices.front(), mesh.vertices.front()};
    for (Vec3 p : mesh.vertices) {
        bounds.low.x = std::min(bounds.low.x, p.x);
        bounds.low.y = std::min(bounds.low.y, p.y);
        bounds.low.z = std::min(bounds.low.z, p.z);
        bounds.high.x = std::max(bounds.high.x, p.x);
        bounds.high.y = std::max(bounds.high.y, p.y);
        bounds.high.z = std::max(bounds.high.z, p.z);
    }
    return bounds;
}

bool in_bounds(Vec3 p, const Bounds& b, double tolerance) {
    return p.x >= b.low.x - tolerance && p.x <= b.high.x + tolerance &&
           p.y >= b.low.y - tolerance && p.y <= b.high.y + tolerance &&
           p.z >= b.low.z - tolerance && p.z <= b.high.z + tolerance;
}

bool bounds_overlap(const Bounds& a, const Bounds& b, double tolerance) {
    return a.low.x <= b.high.x + tolerance && b.low.x <= a.high.x + tolerance &&
           a.low.y <= b.high.y + tolerance && b.low.y <= a.high.y + tolerance &&
           a.low.z <= b.high.z + tolerance && b.low.z <= a.high.z + tolerance;
}

Bounds rectangle_bounds(const Rectangle& rectangle) {
    const auto points = corners(rectangle);
    Bounds bounds{points[0], points[0]};
    for (Vec3 p : points) {
        bounds.low.x = std::min(bounds.low.x, p.x);
        bounds.low.y = std::min(bounds.low.y, p.y);
        bounds.low.z = std::min(bounds.low.z, p.z);
        bounds.high.x = std::max(bounds.high.x, p.x);
        bounds.high.y = std::max(bounds.high.y, p.y);
        bounds.high.z = std::max(bounds.high.z, p.z);
    }
    return bounds;
}

std::uint64_t edge_key(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t low = std::min(a, b);
    const std::uint32_t high = std::max(a, b);
    return (static_cast<std::uint64_t>(low) << 32) | high;
}

bool mesh_is_closed_and_oriented(const Mesh& mesh) {
    struct EdgeUse { int count{0}; int winding{0}; };
    std::unordered_map<std::uint64_t, EdgeUse> edges;
    edges.reserve(mesh.triangles.size() * 3);
    for (const Triangle& t : mesh.triangles) {
        const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> directed{{
            {t.a, t.b}, {t.b, t.c}, {t.c, t.a}}};
        for (auto [a, b] : directed) {
            EdgeUse& use = edges[edge_key(a, b)];
            ++use.count;
            use.winding += a < b ? 1 : -1;
        }
    }
    for (const auto& [key, use] : edges) {
        (void)key;
        if (use.count != 2 || use.winding != 0) return false;
    }
    return true;
}

bool point_on_triangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c, double tolerance) {
    const Vec3 e0 = b - a;
    const Vec3 e1 = c - a;
    const Vec3 n = cross(e0, e1);
    const double n_len = length(n);
    if (n_len <= 0.0 || std::abs(dot(p - a, n) / n_len) > tolerance) return false;
    const double d00 = dot(e0, e0);
    const double d01 = dot(e0, e1);
    const double d11 = dot(e1, e1);
    const double d20 = dot(p - a, e0);
    const double d21 = dot(p - a, e1);
    const double denominator = d00 * d11 - d01 * d01;
    if (denominator <= 0.0) return false;
    const double v = (d11 * d20 - d01 * d21) / denominator;
    const double w = (d00 * d21 - d01 * d20) / denominator;
    const double coordinate_tolerance = tolerance /
        std::max({length(e0), length(e1), 1.0});
    return v >= -coordinate_tolerance && w >= -coordinate_tolerance &&
           v + w <= 1.0 + coordinate_tolerance;
}

enum class PointLocation { Outside, Inside, Boundary };

PointLocation locate_point_in_closed_mesh(const Mesh& mesh, Vec3 p, double tolerance) {
    // The signed solid-angle sum is independent of the ray direction and is
    // ±4*pi inside a consistently oriented closed shell, 0 outside.
    double solid_angle = 0.0;
    for (const Triangle& t : mesh.triangles) {
        const Vec3 a0 = mesh.vertices[t.a];
        const Vec3 b0 = mesh.vertices[t.b];
        const Vec3 c0 = mesh.vertices[t.c];
        if (point_on_triangle(p, a0, b0, c0, tolerance)) return PointLocation::Boundary;
        const Vec3 a = a0 - p;
        const Vec3 b = b0 - p;
        const Vec3 c = c0 - p;
        const double la = length(a);
        const double lb = length(b);
        const double lc = length(c);
        const double numerator = dot(a, cross(b, c));
        const double denominator = la * lb * lc + dot(a, b) * lc +
                                   dot(b, c) * la + dot(c, a) * lb;
        solid_angle += 2.0 * std::atan2(numerator, denominator);
    }
    return std::abs(solid_angle) > 2.0 * std::numbers::pi
        ? PointLocation::Inside : PointLocation::Outside;
}

void check_point_against_optic(PreflightReport& report, const Mesh& mesh,
                               const Bounds& bounds, bool closed, Vec3 point,
                               double tolerance, const std::string& object_id) {
    if (!in_bounds(point, bounds, tolerance)) return;
    if (!closed) {
        add(report, DiagnosticSeverity::Warning, "SOURCE_OPTIC_POSITION_UNCERTAIN",
            object_id, "The optic mesh is open or inconsistently oriented; source containment cannot be verified.");
        return;
    }
    const PointLocation location = locate_point_in_closed_mesh(mesh, point, tolerance);
    if (location != PointLocation::Outside) {
        add(report, DiagnosticSeverity::Error,
            location == PointLocation::Inside ? "SOURCE_INSIDE_OPTIC" : "SOURCE_TOUCHES_OPTIC",
            object_id, "The light source lies inside or on the optic solid.");
    }
}

} // namespace

bool PreflightReport::can_start() const {
    return std::none_of(diagnostics.begin(), diagnostics.end(),
                        [](const Diagnostic& d) { return d.severity == DiagnosticSeverity::Error; });
}

PreflightMeshCache build_preflight_cache(const Mesh& mesh) {
    PreflightMeshCache cache;
    cache.vertex_count = mesh.vertices.size();
    cache.triangle_count = mesh.triangles.size();
    cache.valid = validate_mesh(mesh, cache.error);
    if (cache.valid) {
        const Bounds bounds = mesh_bounds(mesh);
        cache.bounds_low = bounds.low;
        cache.bounds_high = bounds.high;
        cache.closed_and_oriented = mesh_is_closed_and_oriented(mesh);
    }
    return cache;
}

PreflightReport run_preflight(const PreflightScene& scene, const PreflightBudget& budget,
                             PreflightMode mode, const PreflightMeshCache* cache) {
    PreflightReport report;
    PreflightMeshCache final_cache;
    const PreflightMeshCache* mesh_cache = cache;
    if (mode == PreflightMode::Final) {
        final_cache = build_preflight_cache(scene.optic);
        mesh_cache = &final_cache;
    } else if (mesh_cache != nullptr &&
               (mesh_cache->vertex_count != scene.optic.vertices.size() ||
                mesh_cache->triangle_count != scene.optic.triangles.size())) {
        add(report, DiagnosticSeverity::Error, "OPTIC_CACHE_STALE", "optic",
            "The optic mesh changed; rebuild the interactive preflight cache.");
        mesh_cache = nullptr;
    } else if (mesh_cache == nullptr) {
        add(report, DiagnosticSeverity::Info, "OPTIC_NOT_EVALUATED", "optic",
            "Interactive preflight has no mesh cache; optic validity and source containment are not evaluated.");
    }
    const bool optic_valid = mesh_cache != nullptr && mesh_cache->valid;
    if (mesh_cache != nullptr && !optic_valid) {
        add(report, DiagnosticSeverity::Error, "OPTIC_INVALID_MESH", "optic", mesh_cache->error);
    } else if (scene.optic.triangles.size() > budget.max_mesh_triangles) {
        add(report, DiagnosticSeverity::Warning, "MESH_EXCEEDS_BUDGET", "optic",
            "The optic mesh exceeds the configured triangle budget.");
    }

    const bool source_center_valid = finite(scene.source.center);
    if (!source_center_valid) {
        add(report, DiagnosticSeverity::Error, "SOURCE_INVALID_POSITION", "source",
            "The source position must contain finite coordinates.");
    }
    bool source_rectangle_valid = false;
    Rectangle source_rectangle{};
    const bool distant = scene.source.kind == LightSourceKind::SolarDisk;
    if (distant && !valid_basis(scene.source.u, scene.source.v)) {
        add(report, DiagnosticSeverity::Error, "SOURCE_INVALID_DIRECTION", "source",
            "The solar source needs a finite orthonormal basis.");
    }
    if (scene.source.kind == LightSourceKind::Rectangle) {
        source_rectangle_valid = source_center_valid && valid_basis(scene.source.u, scene.source.v) &&
            positive_finite(scene.source.width_mm) && positive_finite(scene.source.height_mm);
        if (!source_rectangle_valid) {
            add(report, DiagnosticSeverity::Error, "SOURCE_INVALID_RECTANGLE", "source",
                "The area source needs positive dimensions and a finite orthonormal basis.");
        } else {
            source_rectangle = make_rectangle(scene.source.center, scene.source.u, scene.source.v,
                                              scene.source.width_mm, scene.source.height_mm);
        }
    }

    std::vector<bool> receiver_valid(scene.receivers.size(), false);
    std::vector<Rectangle> rectangles(scene.receivers.size());
    std::unordered_set<std::string> receiver_ids;
    if (scene.receivers.empty()) {
        add(report, DiagnosticSeverity::Error, "NO_RECEIVERS", "scene",
            "At least one projection surface is required.");
    }
    for (std::size_t i = 0; i < scene.receivers.size(); ++i) {
        const RectReceiver& receiver = scene.receivers[i];
        const std::string object_id = receiver.id.empty()
            ? "receiver[" + std::to_string(i) + "]" : receiver.id;
        if (!receiver.id.empty() && !receiver_ids.insert(receiver.id).second) {
            add(report, DiagnosticSeverity::Error, "RECEIVER_DUPLICATE_ID", object_id,
                "Projection surfaces must have unique IDs.");
        }
        receiver_valid[i] = finite(receiver.center) && valid_basis(receiver.u, receiver.v) &&
            positive_finite(receiver.width_mm) && positive_finite(receiver.height_mm);
        if (!receiver_valid[i]) {
            add(report, DiagnosticSeverity::Error, "RECEIVER_INVALID_GEOMETRY", object_id,
                "The receiver needs positive dimensions and a finite orthonormal basis.");
            continue;
        }
        rectangles[i] = make_rectangle(receiver.center, receiver.u, receiver.v,
                                       receiver.width_mm, receiver.height_mm);
        if (receiver.image_width_px == 0 || receiver.image_height_px == 0) {
            add(report, DiagnosticSeverity::Warning, "RECEIVER_NO_IMAGE_RESOLUTION", object_id,
                "No target-image resolution was supplied for this receiver.");
        }
    }

    double scene_scale = 1.0;
    if (optic_valid) {
        const Bounds bounds{mesh_cache->bounds_low, mesh_cache->bounds_high};
        scene_scale = std::max({scene_scale, length(bounds.high - bounds.low)});
    }
    for (const RectReceiver& receiver : scene.receivers) {
        if (positive_finite(receiver.width_mm)) scene_scale = std::max(scene_scale, receiver.width_mm);
        if (positive_finite(receiver.height_mm)) scene_scale = std::max(scene_scale, receiver.height_mm);
    }
    const double tolerance = 1e-8 * scene_scale + 1e-7;

    for (std::size_t i = 0; i < scene.receivers.size(); ++i) {
        if (!receiver_valid[i]) continue;
        const std::string object_id = scene.receivers[i].id.empty()
            ? "receiver[" + std::to_string(i) + "]" : scene.receivers[i].id;
        if (!distant && source_center_valid && point_in_rectangle(scene.source.center, rectangles[i], tolerance)) {
            add(report, DiagnosticSeverity::Error, "SOURCE_ON_RECEIVER", object_id,
                "The light source lies on a projection surface.");
        }
        if (source_rectangle_valid && rectangles_intersect(source_rectangle, rectangles[i], tolerance)) {
            add(report, DiagnosticSeverity::Error, "SOURCE_OVERLAPS_RECEIVER", object_id,
                "The area source intersects a projection surface.");
        }
        for (std::size_t j = i + 1; j < scene.receivers.size(); ++j) {
            if (!receiver_valid[j]) continue;
            if (rectangles_intersect(rectangles[i], rectangles[j], tolerance)) {
                const std::string other_id = scene.receivers[j].id.empty()
                    ? "receiver[" + std::to_string(j) + "]" : scene.receivers[j].id;
                add(report, DiagnosticSeverity::Error, "RECEIVERS_INTERSECT", object_id,
                    "Projection surface intersects " + other_id + ".");
            }
        }
    }

    if (optic_valid && source_center_valid) {
        const Bounds bounds{mesh_cache->bounds_low, mesh_cache->bounds_high};
        const bool closed = mesh_cache->closed_and_oriented;
        if (!closed) {
            add(report, DiagnosticSeverity::Warning, "OPTIC_NOT_CLOSED_OR_ORIENTED", "optic",
                "The optic mesh is open, non-manifold, or inconsistently oriented.");
        }
        if (!distant && mode == PreflightMode::Final) {
            check_point_against_optic(report, scene.optic, bounds, closed,
                                      scene.source.center, tolerance, "source");
        } else if (!distant && in_bounds(scene.source.center, bounds, tolerance)) {
            add(report, DiagnosticSeverity::Warning, "SOURCE_OPTIC_POSITION_UNCHECKED", "source",
                "The source lies within the optic bounds; exact containment is deferred to final preflight.");
        }
        if (source_rectangle_valid) {
            if (bounds_overlap(bounds, rectangle_bounds(source_rectangle), tolerance)) {
                add(report, DiagnosticSeverity::Warning, "SOURCE_OPTIC_BOUNDS_OVERLAP", "source",
                    "The area-source bounds overlap the optic bounds; this does not prove an intersection.");
            }
            for (Vec3 p : corners(source_rectangle)) {
                if (mode == PreflightMode::Final) {
                    check_point_against_optic(report, scene.optic, bounds, closed,
                                              p, tolerance, "source");
                } else if (in_bounds(p, bounds, tolerance)) {
                    add(report, DiagnosticSeverity::Warning, "SOURCE_OPTIC_POSITION_UNCHECKED", "source",
                        "Part of the area source lies within the optic bounds; exact containment is deferred.");
                    break;
                }
            }
            // This is only a broadphase warning: neither an AABB overlap nor
            // a sparse sample proves the emitter intersects the optic.
            for (Vec3 p : corners(source_rectangle)) {
                if (in_bounds(p, bounds, tolerance) && !closed) {
                    add(report, DiagnosticSeverity::Warning, "SOURCE_OPTIC_PROXIMITY", "source",
                        "Part of the area source lies near an optic whose containment is uncertain.");
                    break;
                }
            }
            const Vec3 optic_center = (bounds.low + bounds.high) * 0.5;
            if (dot(source_rectangle.normal, optic_center - scene.source.center) <= 0.0) {
                add(report, DiagnosticSeverity::Warning, "SOURCE_FACING_AWAY", "source",
                    "The area-source normal points away from the optic centre.");
            }
        }
        const Vec3 optic_center = (bounds.low + bounds.high) * 0.5;
        for (std::size_t i = 0; i < scene.receivers.size(); ++i) {
            if (!receiver_valid[i]) continue;
            const std::string object_id = scene.receivers[i].id.empty()
                ? "receiver[" + std::to_string(i) + "]" : scene.receivers[i].id;
            const Vec3 toward_receiver = scene.receivers[i].center - optic_center;
            const double distance = length(toward_receiver);
            if (distance <= tolerance) {
                add(report, DiagnosticSeverity::Warning, "RECEIVER_NEAR_OPTIC_CENTRE", object_id,
                    "The receiver centre is too close to the optic centre for a useful quick optical estimate.");
                continue;
            }
            const double face_factor = std::abs(dot(rectangles[i].normal, toward_receiver / distance));
            if (face_factor < 0.05) {
                add(report, DiagnosticSeverity::Warning, "RECEIVER_GRAZING", object_id,
                    "The receiver is nearly edge-on from the optic centre; projected flux may be low.");
            }
            for (std::size_t j = 0; j < scene.receivers.size(); ++j) {
                if (j == i || !receiver_valid[j]) continue;
                if (segment_intersects_rectangle(optic_center, scene.receivers[i].center,
                                                 rectangles[j], tolerance)) {
                    add(report, DiagnosticSeverity::Warning, "RECEIVER_CENTRE_OCCLUDED", object_id,
                        "Another receiver blocks the centre-to-centre line from the optic; a full ray trace is needed.");
                    break;
                }
            }
        }
    }

    if (scene.angles_degrees.empty()) {
        add(report, DiagnosticSeverity::Error, "NO_ANGLES", "scene",
            "At least one rotation angle is required.");
    }
    std::vector<double> canonical_angles;
    canonical_angles.reserve(scene.angles_degrees.size());
    for (double angle : scene.angles_degrees) {
        if (!std::isfinite(angle)) {
            add(report, DiagnosticSeverity::Error, "ANGLE_NONFINITE", "scene",
                "Rotation angles must be finite.");
            continue;
        }
        double canonical = std::fmod(angle, 360.0);
        if (canonical < 0.0) canonical += 360.0;
        const bool duplicate = std::any_of(canonical_angles.begin(), canonical_angles.end(),
            [canonical](double prior) { return std::abs(prior - canonical) < 1e-9 ||
                std::abs(std::abs(prior - canonical) - 360.0) < 1e-9; });
        if (duplicate) {
            add(report, DiagnosticSeverity::Warning, "ANGLE_DUPLICATE", "scene",
                "A rotation angle duplicates another orientation modulo 360 degrees.");
        } else {
            canonical_angles.push_back(canonical);
        }
    }

    if (scene.rays_per_iteration == 0 || scene.iterations == 0) {
        add(report, DiagnosticSeverity::Error, "INVALID_SOLVER_BUDGET", "solver",
            "Ray count and iteration count must both be positive.");
    } else {
        const std::uint64_t angle_count = static_cast<std::uint64_t>(scene.angles_degrees.size());
        std::uint64_t per_iteration = 0;
        if (!checked_product(scene.rays_per_iteration, angle_count, per_iteration) ||
            !checked_product(per_iteration, scene.iterations, report.estimated_primary_rays) ||
            !checked_product(report.estimated_primary_rays,
                             static_cast<std::uint64_t>(scene.receivers.size()),
                             report.estimated_ray_screen_tests)) {
            add(report, DiagnosticSeverity::Error, "BUDGET_OVERFLOW", "solver",
                "Estimated ray count exceeds the numeric range.");
        } else {
            if (report.estimated_primary_rays > budget.max_primary_rays ||
                report.estimated_ray_screen_tests > budget.max_ray_screen_tests) {
                add(report, DiagnosticSeverity::Warning, "BUDGET_HIGH", "solver",
                    "The scene exceeds the configured work estimate; use a bounded worker and coarse-to-fine settings.");
            }
        }
    }
    return report;
}

} // namespace caustic
