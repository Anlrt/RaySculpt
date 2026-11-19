#pragma once
#include <filesystem>

namespace caustic {
void trace_scene(const std::filesystem::path& settings, const std::filesystem::path& profile,
                 const std::filesystem::path& out, double angle=0.);
void fit_scene(const std::filesystem::path& input, const std::filesystem::path& out);
void trace_photo(const std::filesystem::path& profile, const std::filesystem::path& target,
                 const std::filesystem::path& out, int rays, int max_mesh_cells, int seed);
}
