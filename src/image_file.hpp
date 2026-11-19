#pragma once

#include <filesystem>
#include <span>
#include <vector>

namespace caustic {

struct LinearImage {
    int width{};
    int height{};
    std::vector<double> pixels; // Top row first. Zero means no light.
};

LinearImage load_target_image(const std::filesystem::path& path);
LinearImage resize_target(const LinearImage& image, int width, int height);
void save_projection_image(const std::filesystem::path& path, int width, int height,
                           std::span<const double> pixels, double exposure);

} // namespace caustic
