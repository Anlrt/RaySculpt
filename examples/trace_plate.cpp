#include "diagnostics.hpp"
#include "optics.hpp"
#include "surface_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        const std::filesystem::path output = argc > 1 ? argv[1] : "caustic_demo.pgm";
        caustic::HeightField plate;
        plate.samples_x = 256;
        plate.samples_y = 256;
        plate.width_mm = 40;
        plate.height_mm = 40;
        plate.thickness_mm = 5;
        plate.offsets_mm.resize(static_cast<std::size_t>(plate.samples_x) * plate.samples_y);
        for (std::uint32_t y = 0; y < plate.samples_y; ++y) {
            for (std::uint32_t x = 0; x < plate.samples_x; ++x) {
                const double u = static_cast<double>(x) / (plate.samples_x - 1);
                const double v = static_cast<double>(y) / (plate.samples_y - 1);
                plate.offsets_mm[static_cast<std::size_t>(y) * plate.samples_x + x] =
                    0.30 * std::cos(2.0 * std::numbers::pi * u) *
                           std::cos(2.0 * std::numbers::pi * v);
            }
        }

        std::filesystem::path solid_path = output;
        solid_path.replace_extension(".stl");
        std::string error;
        const auto solid = caustic::make_height_field_solid(plate);
        if (!caustic::save_binary_stl(solid_path, solid, error))
            throw std::runtime_error(error);

        caustic::LightSource source;
        source.center = {0, 0, -200};
        caustic::RectReceiver screen;
        screen.id = "main";
        screen.center = {0, 0, 150};
        screen.width_mm = 100;
        screen.height_mm = 100;
        screen.image_width_px = 256;
        screen.image_height_px = 256;
        caustic::TraceConfig config;
        config.ray_count = 5'000'000;
        config.max_seconds = 10;
        const auto result = caustic::trace_caustic(plate, {}, source, {screen}, config);
        const auto& irradiance = result.images.at(0).values;
        const double peak = *std::max_element(irradiance.begin(), irradiance.end());

        std::ofstream image(output, std::ios::binary);
        if (!image) throw std::runtime_error("cannot open output image");
        image << "P5\n256 256\n255\n";
        for (double value : irradiance) {
            const auto byte = static_cast<unsigned char>(
                std::lround(255.0 * (peak > 0.0 ? std::sqrt(value / peak) : 0.0)));
            image.write(reinterpret_cast<const char*>(&byte), 1);
        }
        image.close();
        if (!image) throw std::runtime_error("cannot write output image");

        if (!caustic::append_trace_log(output.string() + ".jsonl", result, 1, error))
            throw std::runtime_error(error);
        std::cout << "processed=" << result.stats.processed
                  << " hits=" << result.stats.receiver_hit
                  << " escaped=" << result.stats.side_escape
                  << " model=" << solid_path.string()
                  << " elapsed=" << result.elapsed_seconds << " s\n";
        return result.stop_reason == caustic::TraceStopReason::Completed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "trace demo failed: " << error.what() << '\n';
        return 1;
    }
}
