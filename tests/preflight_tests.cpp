#include "preflight.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace caustic;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool has_code(const PreflightReport& report, const std::string& code,
              DiagnosticSeverity severity) {
    return std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
        [&](const Diagnostic& d) { return d.code == code && d.severity == severity; });
}

PreflightScene valid_scene() {
    PreflightScene scene;
    scene.optic = make_cube(20.0);
    scene.source.center = {0.0, 0.0, 50.0};
    scene.receivers.push_back({"screen", {0.0, 0.0, -100.0},
                               {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0},
                               60.0, 60.0, 128, 128});
    scene.angles_degrees = {0.0, 20.0};
    scene.rays_per_iteration = 1000;
    scene.iterations = 10;
    return scene;
}

void test_valid_scene_and_estimate() {
    const PreflightReport report = run_preflight(valid_scene());
    require(report.can_start(), "valid scene must pass");
    require(report.estimated_primary_rays == 20'000, "primary-ray estimate is wrong");
    require(report.estimated_ray_screen_tests == 20'000, "screen-test estimate is wrong");
}

void test_receiver_geometry() {
    auto scene = valid_scene();
    scene.receivers[0].width_mm = 0.0;
    require(has_code(run_preflight(scene), "RECEIVER_INVALID_GEOMETRY", DiagnosticSeverity::Error),
            "zero-size receiver must fail");

    scene = valid_scene();
    scene.receivers[0].v = {1.0, 0.0, 0.0};
    require(has_code(run_preflight(scene), "RECEIVER_INVALID_GEOMETRY", DiagnosticSeverity::Error),
            "non-orthogonal basis must fail");

    scene = valid_scene();
    scene.receivers[0].height_mm = std::numeric_limits<double>::infinity();
    require(has_code(run_preflight(scene), "RECEIVER_INVALID_GEOMETRY", DiagnosticSeverity::Error),
            "nonfinite size must fail");
}

void test_receiver_collisions() {
    auto scene = valid_scene();
    scene.receivers.push_back({"perpendicular", {0.0, 0.0, -100.0},
                               {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0},
                               20.0, 20.0, 64, 64});
    require(has_code(run_preflight(scene), "RECEIVERS_INTERSECT", DiagnosticSeverity::Error),
            "perpendicular crossing receivers must fail");

    scene = valid_scene();
    scene.receivers.push_back({"coplanar", {20.0, 0.0, -100.0},
                               {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0},
                               20.0, 20.0, 64, 64});
    require(has_code(run_preflight(scene), "RECEIVERS_INTERSECT", DiagnosticSeverity::Error),
            "coplanar overlap must fail");

    scene = valid_scene();
    scene.receivers.push_back({"distant", {0.0, 0.0, -150.0},
                               {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0},
                               20.0, 20.0, 64, 64});
    require(!has_code(run_preflight(scene), "RECEIVERS_INTERSECT", DiagnosticSeverity::Error),
            "disjoint perpendicular receivers should pass geometry check");
}

void test_source_errors() {
    auto scene = valid_scene();
    scene.source.center = {0.0, 0.0, 0.0};
    require(has_code(run_preflight(scene), "SOURCE_INSIDE_OPTIC", DiagnosticSeverity::Error),
            "point inside closed optic must fail");

    scene = valid_scene();
    scene.source.center = {10.0, 0.0, 0.0};
    require(has_code(run_preflight(scene), "SOURCE_TOUCHES_OPTIC", DiagnosticSeverity::Error),
            "point on optic surface must fail");

    scene = valid_scene();
    scene.source.center = {0.0, 0.0, -100.0};
    require(has_code(run_preflight(scene), "SOURCE_ON_RECEIVER", DiagnosticSeverity::Error),
            "point on receiver must fail");

    scene = valid_scene();
    scene.source.kind = LightSourceKind::Rectangle;
    scene.source.width_mm = 10.0;
    scene.source.height_mm = 10.0;
    scene.source.v = scene.source.u;
    require(has_code(run_preflight(scene), "SOURCE_INVALID_RECTANGLE", DiagnosticSeverity::Error),
            "invalid area source basis must fail");

    scene = valid_scene();
    scene.source.kind = LightSourceKind::Rectangle;
    scene.source.center = {0.0, 0.0, -100.0};
    scene.source.width_mm = 10.0;
    scene.source.height_mm = 10.0;
    require(has_code(run_preflight(scene), "SOURCE_OVERLAPS_RECEIVER", DiagnosticSeverity::Error),
            "area source overlapping receiver must fail");
}

void test_mesh_and_optical_warning() {
    auto scene = valid_scene();
    scene.optic.triangles.pop_back();
    const PreflightReport open = run_preflight(scene);
    require(open.can_start(), "open optic is a warning, not a proven optical impossibility");
    require(has_code(open, "OPTIC_NOT_CLOSED_OR_ORIENTED", DiagnosticSeverity::Warning),
            "open mesh warning missing");

    scene = valid_scene();
    scene.optic.triangles[0] = {0, 0, 1};
    require(has_code(run_preflight(scene), "OPTIC_INVALID_MESH", DiagnosticSeverity::Error),
            "degenerate optic triangle must fail");

    scene = valid_scene();
    scene.receivers[0].u = {0.0, 0.0, 1.0};
    scene.receivers[0].v = {0.0, 1.0, 0.0};
    require(has_code(run_preflight(scene), "RECEIVER_GRAZING", DiagnosticSeverity::Warning),
            "near edge-on receiver should warn, not fail");
}

void test_angles_and_budget() {
    auto scene = valid_scene();
    scene.angles_degrees = {0.0, 360.0};
    require(has_code(run_preflight(scene), "ANGLE_DUPLICATE", DiagnosticSeverity::Warning),
            "duplicate angle warning missing");

    scene = valid_scene();
    scene.angles_degrees = {std::numeric_limits<double>::quiet_NaN()};
    require(has_code(run_preflight(scene), "ANGLE_NONFINITE", DiagnosticSeverity::Error),
            "nonfinite angle must fail");

    scene = valid_scene();
    scene.rays_per_iteration = 0;
    require(has_code(run_preflight(scene), "INVALID_SOLVER_BUDGET", DiagnosticSeverity::Error),
            "zero ray budget must fail");

    scene = valid_scene();
    scene.rays_per_iteration = 100;
    scene.iterations = 100;
    PreflightBudget budget;
    budget.max_primary_rays = 1;
    require(has_code(run_preflight(scene, budget), "BUDGET_HIGH", DiagnosticSeverity::Warning),
            "high resource estimate must warn");

    scene = valid_scene();
    scene.rays_per_iteration = std::numeric_limits<std::uint64_t>::max();
    require(has_code(run_preflight(scene), "BUDGET_OVERFLOW", DiagnosticSeverity::Error),
            "integer-overflow budget must fail");
}

void test_interactive_cache_defers_expensive_containment() {
    auto scene = valid_scene();
    const PreflightMeshCache cache = build_preflight_cache(scene.optic);
    require(cache.valid && cache.closed_and_oriented, "cube cache should be valid and closed");
    scene.source.center = {0.0, 0.0, 0.0};
    const PreflightReport interactive = run_preflight(scene, {}, PreflightMode::Interactive, &cache);
    require(!has_code(interactive, "SOURCE_INSIDE_OPTIC", DiagnosticSeverity::Error),
            "interactive mode must defer solid-angle containment");
    require(has_code(interactive, "SOURCE_OPTIC_POSITION_UNCHECKED", DiagnosticSeverity::Warning),
            "interactive mode must label deferred containment");
    require(has_code(run_preflight(scene), "SOURCE_INSIDE_OPTIC", DiagnosticSeverity::Error),
            "final mode must establish source containment");

    scene.optic.triangles.pop_back();
    require(has_code(run_preflight(scene, {}, PreflightMode::Interactive, &cache),
                     "OPTIC_CACHE_STALE", DiagnosticSeverity::Error),
            "interactive mode must reject a cache with changed mesh counts");

    const PreflightReport no_cache = run_preflight(scene, {}, PreflightMode::Interactive);
    require(has_code(no_cache, "OPTIC_NOT_EVALUATED", DiagnosticSeverity::Info),
            "interactive mode without a cache must state its limitation");
}

} // namespace

int main() {
    try {
        test_valid_scene_and_estimate();
        test_receiver_geometry();
        test_receiver_collisions();
        test_source_errors();
        test_mesh_and_optical_warning();
        test_angles_and_budget();
        test_interactive_cache_defers_expensive_containment();
        std::cout << "Preflight tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Preflight test failed: " << e.what() << '\n';
        return 1;
    }
}
