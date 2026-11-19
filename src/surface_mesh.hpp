#pragma once

#include "geometry.hpp"
#include "optics.hpp"

#include <filesystem>
#include <string>

namespace caustic {

// Convert a height field into a closed solid: flat entrance at z=0, sampled
// exit at z=thickness+offset, and four side walls. Coordinates are in mm.
// Subdivide each cell to approximate the optical surface with triangles.
Mesh make_height_field_solid(const HeightField& plate, unsigned cell_subdivisions = 1);

struct SplineTessellation {
    unsigned subdivisions{};
    double height_error_mm{}; // Upper bound on the vertical approximation error.
    double normal_error_deg{}; // Upper bound over the optical face.
};

SplineTessellation tessellate_to_tolerance(const HeightField& spline,
                                          double height_mm, double normal_degrees);

// Binary STL has no units. This project always writes positions in mm.
bool save_binary_stl(const std::filesystem::path& path, const Mesh& mesh,
                     std::string& error);

} // namespace caustic
