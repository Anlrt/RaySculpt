#pragma once

#include <filesystem>
#include <vector>

namespace caustic {

struct PlateTarget {
    double angle_degrees{};
    std::filesystem::path image;
};

struct PlateDesign {
    std::vector<PlateTarget> targets;
    std::filesystem::path output;
    int grid = 9;
    int resolution = 32;
    int rays = 80'000;
    int evaluations = 700;
    int threads = 4;
    double seconds = 30;
    double plate_mm = 40;
    double thickness_mm = 5;
    double max_relief_mm = 1.5;
    double index = 1.49;
    double source_mm = 200;
    double screen_distance_mm = 150;
    double screen_mm = 70;
    double stl_height_mm = 0.005;
    double stl_normal_deg = 1.0;
    bool write_stl = true;
};

void design_plate(const PlateDesign& settings);

} // namespace caustic
