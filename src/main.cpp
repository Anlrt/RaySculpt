#include "geometry.hpp"
#include "plate_design.hpp"
#include "photo_trace.hpp"
#include "target_renderer.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using ArgString = std::wstring;
using ArgChar = wchar_t;
#define CAUSTIC_ENTRY wmain
#else
using ArgString = std::string;
using ArgChar = char;
#define CAUSTIC_ENTRY main
#endif

bool is_option(const ArgString& value, std::string_view option) {
    return value == ArgString(option.begin(), option.end());
}

std::filesystem::path as_path(const ArgString& value) { return std::filesystem::path(value); }
double as_double(const ArgString& value) {
    std::size_t used = 0;
    const double parsed = std::stod(value, &used);
    if (used != value.size() || !std::isfinite(parsed))
        throw std::invalid_argument("invalid numeric value");
    return parsed;
}
int as_int(const ArgString& value) {
    std::size_t used = 0;
    const int parsed = std::stoi(value, &used);
    if (used != value.size()) throw std::invalid_argument("invalid integer value");
    return parsed;
}

void run_design_command(int argc, ArgChar** argv) {
    caustic::PlateDesign settings;
    for (int i = 2; i < argc; ++i) {
        const ArgString option = argv[i];
        if (i + 1 >= argc) throw std::invalid_argument("missing design option value");
        const ArgString value = argv[++i];
        if (is_option(option, "--target")) {
            settings.targets.push_back({0, as_path(value)});
        } else if (is_option(option, "--frame")) {
            if (i + 1 >= argc) throw std::invalid_argument("--frame needs an angle and an image");
            settings.targets.push_back({as_double(value), as_path(argv[++i])});
        } else if (is_option(option, "--out")) settings.output = as_path(value);
        else if (is_option(option, "--grid")) settings.grid = as_int(value);
        else if (is_option(option, "--resolution")) settings.resolution = as_int(value);
        else if (is_option(option, "--rays")) settings.rays = as_int(value);
        else if (is_option(option, "--evals")) settings.evaluations = as_int(value);
        else if (is_option(option, "--threads")) settings.threads = as_int(value);
        else if (is_option(option, "--seconds")) settings.seconds = as_double(value);
        else if (is_option(option, "--plate-mm")) settings.plate_mm = as_double(value);
        else if (is_option(option, "--thickness-mm")) settings.thickness_mm = as_double(value);
        else if (is_option(option, "--max-relief-mm")) settings.max_relief_mm = as_double(value);
        else if (is_option(option, "--index")) settings.index = as_double(value);
        else if (is_option(option, "--source-mm")) settings.source_mm = as_double(value);
        else if (is_option(option, "--screen-distance-mm")) settings.screen_distance_mm = as_double(value);
        else if (is_option(option, "--screen-mm")) settings.screen_mm = as_double(value);
        else if (is_option(option, "--stl-height-mm")) settings.stl_height_mm = as_double(value);
        else if (is_option(option, "--stl-normal-deg")) settings.stl_normal_deg = as_double(value);
        else if (is_option(option, "--write-stl")) settings.write_stl = as_int(value)!=0;
        else throw std::invalid_argument("unknown design option");
    }
    caustic::design_plate(settings);
}

int export_command(int argc, ArgChar** argv, bool design) {
    const auto bin = std::filesystem::absolute(argv[0]).parent_path();
    auto script = bin / "tools" / "export_plate.py";
    if (!std::filesystem::is_regular_file(script))
        script = bin.parent_path().parent_path() / "tools" / "export_plate.py";
    if (!std::filesystem::is_regular_file(script))
        script = std::filesystem::path(CAUSTIC_TOOLS_DIR) / "export_plate.py";
#ifdef _WIN32
    std::vector<ArgString> args{L"python",script.wstring()};
#else
    std::vector<ArgString> args{"python3",script.string()};
#endif
    if (design) {
        const std::string mode = "_design";
        args.emplace_back(mode.begin(),mode.end());
        args.emplace_back(argv[0]);
    }
    for (int i=2; i<argc; ++i) args.emplace_back(argv[i]);
#ifdef _WIN32
    // The CRT joins argv into a Windows command line. Preserve literals
    // including embedded quotes and trailing backslashes without a shell.
    for (auto& arg : args) {
        std::wstring quoted = L"\"";
        std::size_t slashes = 0;
        for (wchar_t ch : arg) {
            if (ch==L'\\') { ++slashes; continue; }
            quoted.append(ch==L'"' ? 2*slashes+1 : slashes,L'\\');
            quoted.push_back(ch);
            slashes=0;
        }
        quoted.append(2*slashes,L'\\');
        arg = quoted+L"\"";
    }
    std::vector<const wchar_t*> pointers;
    for (const auto& arg : args) pointers.push_back(arg.c_str());
    pointers.push_back(nullptr);
    const auto status = _wspawnvp(_P_WAIT,L"python",pointers.data());
    if (status!=-1) return static_cast<int>(status);
#else
    std::vector<char*> pointers;
    for (auto& arg : args) pointers.push_back(arg.data());
    pointers.push_back(nullptr);
    execvp(pointers[0],pointers.data());
#endif
    throw std::runtime_error("cannot launch Python for model export");
}

void save_pgm(const std::filesystem::path& path, const caustic::GrayImage& image) {
    const auto parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot open output image");
    out << "P5\n" << image.width << ' ' << image.height << "\n255\n";
    out.write(reinterpret_cast<const char*>(image.pixels.data()),
              static_cast<std::streamsize>(image.pixels.size()));
    if (!out) throw std::runtime_error("cannot write output image");
}

void print_usage() {
    std::cout
        << "Caustic CAD core\n"
        << "Usage:\n"
        << "  caustic_cli render-target (--obj FILE | --cube MM | --sphere MM) --out FILE\n"
        << "      [--angle DEG] [--elevation DEG] [--width PX] [--height PX]\n"
        << "      [--ambient WEIGHT] [--depth WEIGHT] [--edges WEIGHT]\n"
        << "  caustic_cli render-turntable (--obj FILE | --cube MM | --sphere MM) --out DIR\n"
        << "      [--views COUNT] [--elevation DEG] [--width PX] [--height PX]\n"
        << "  caustic_cli design-plate (--target FILE | --frame DEG FILE [--frame DEG FILE ...]) --out NEW_DIR\n"
        << "      [--plate-mm MM] [--thickness-mm MM] [--max-relief-mm MM] [--index N]\n"
        << "      [--source-mm MM] [--screen-distance-mm MM] [--screen-mm MM]\n"
        << "      [--grid N] [--resolution PX] [--rays N] [--evals N] [--seconds N] [--threads N]\n"
        << "      [--stl-height-mm MM] [--stl-normal-deg DEG] [--formats step,brep,stl,3mf,obj]\n"
        << "  caustic_cli export-plate DESIGN --out FILE_OR_NEW_DIR [--formats step,brep,stl,3mf,obj] [--cells N]\n"
        << "  caustic_cli trace-photo PROFILE TARGET.pgm NEW_DIR RAYS MAX_MESH_CELLS [SEED]\n"
        << "  caustic_cli trace-scene SETTINGS PROFILE_OR_DASH NEW_DIR [ANGLE [VALIDATION_SEED [CANCEL_FILE]]]\n"
        << "  caustic_cli fit-scene INPUT_DIR NEW_DIR\n"
        << "  Photo design: python tools/design_photo.py IMAGE --out NEW_DIR\n";
}

} // namespace

int CAUSTIC_ENTRY(int argc, ArgChar** argv) {
    try {
        if (argc < 2) {
            print_usage();
            return 2;
        }
        const ArgString command = argv[1];
        if (is_option(command, "trace-scene")) {
            int max_args = 8;
            std::uint32_t profile_threads = 0;
#ifdef CAUSTIC_REFERENCE_TRACE
            max_args = 9;
            if (argc == 9) {
                const int count = as_int(argv[8]);
                if (count < 1 || count > 24) throw std::invalid_argument("profile threads outside [1,24]");
                profile_threads = static_cast<std::uint32_t>(count);
            }
#endif
            if (argc < 5 || argc > max_args) throw std::invalid_argument("invalid trace-scene arguments");
            std::optional<std::uint64_t> seed;
            if (argc >= 7) {
                const auto value = as_int(argv[6]);
                if (value < 0) throw std::invalid_argument("validation seed must be nonnegative");
                seed = static_cast<std::uint64_t>(value);
            }
            caustic::trace_scene(as_path(argv[2]), as_path(argv[3]), as_path(argv[4]),
                                 argc >= 6 ? as_double(argv[5]) : 0., seed,
                                 argc >= 8 ? as_path(argv[7]) : std::filesystem::path{}, profile_threads);
            return 0;
        }
        if (is_option(command, "fit-scene")) {
            if (argc!=4) throw std::invalid_argument("fit-scene needs a prepared scene and an output directory");
            caustic::fit_scene(as_path(argv[2]),as_path(argv[3]));
            return 0;
        }
        if (is_option(command, "trace-photo")) {
            if (argc != 7 && argc != 8) throw std::invalid_argument("trace-photo needs profile, target, out, rays, mesh cells and optional seed");
            caustic::trace_photo(as_path(argv[2]), as_path(argv[3]), as_path(argv[4]),
                                as_int(argv[5]), as_int(argv[6]), argc == 8 ? as_int(argv[7]) : 0x70ca4e31);
            return 0;
        }
        if (is_option(command, "--help")) {
            print_usage();
            return 0;
        }
        if (is_option(command, "design-plate")) {
            for (int i=2; i<argc; ++i)
                if (is_option(argv[i],"--formats")) return export_command(argc,argv,true);
            run_design_command(argc, argv);
            return 0;
        }
        const bool turntable = is_option(command, "render-turntable");
        if (is_option(command,"export-plate")) return export_command(argc,argv,false);
        if (!turntable && !is_option(command, "render-target")) {
            print_usage();
            return 2;
        }

        caustic::RenderSettings settings{};
        settings.width = 512;
        settings.height = 512;
        settings.azimuth_degrees = 0.0;
        settings.elevation_degrees = 15.0;
        settings.depth_weight = 0.15;
        settings.ambient_weight = 0.22;
        settings.edge_weight = 0.18;

        caustic::Mesh mesh;
        bool have_mesh = false;
        std::filesystem::path output;
        int views = 12;
        for (int i = 2; i < argc; ++i) {
            const ArgString option = argv[i];
            if (i + 1 >= argc) throw std::invalid_argument("missing option value");
            const ArgString value = argv[++i];
            if (is_option(option, "--obj")) {
                std::string error;
                if (!caustic::load_obj(as_path(value), mesh, error))
                    throw std::runtime_error("OBJ import: " + error);
                have_mesh = true;
            } else if (is_option(option, "--cube")) {
                mesh = caustic::make_cube(as_double(value));
                have_mesh = true;
            } else if (is_option(option, "--sphere")) {
                mesh = caustic::make_uv_sphere(as_double(value));
                have_mesh = true;
            } else if (is_option(option, "--out")) {
                output = as_path(value);
            } else if (is_option(option, "--angle")) {
                settings.azimuth_degrees = as_double(value);
            } else if (is_option(option, "--elevation")) {
                settings.elevation_degrees = as_double(value);
            } else if (is_option(option, "--width")) {
                settings.width = as_int(value);
            } else if (is_option(option, "--height")) {
                settings.height = as_int(value);
            } else if (is_option(option, "--views")) {
                views = as_int(value);
            } else if (is_option(option, "--ambient")) {
                settings.ambient_weight = as_double(value);
            } else if (is_option(option, "--depth")) {
                settings.depth_weight = as_double(value);
            } else if (is_option(option, "--edges")) {
                settings.edge_weight = as_double(value);
            } else {
                throw std::invalid_argument("unknown option");
            }
        }
        if (!have_mesh || output.empty()) throw std::invalid_argument("model and --out are required");
        if (views < 1 || views > 360) throw std::invalid_argument("views must be in [1, 360]");
        if (turntable) {
            std::filesystem::create_directories(output);
            const double first_angle = settings.azimuth_degrees;
            for (int frame = 0; frame < views; ++frame) {
                settings.azimuth_degrees = first_angle + 360.0 * frame / views;
                const auto image = caustic::render_target(mesh, settings);
                std::string name = "view_" + std::to_string(1000 + frame).substr(1) + ".pgm";
                save_pgm(output / name, image);
                std::cerr << "rendered view " << frame + 1 << '/' << views << '\n';
            }
            std::cout << "wrote " << views << " monochrome views\n";
        } else {
            const auto image = caustic::render_target(mesh, settings);
            save_pgm(output, image);
            std::cout << "wrote monochrome target image\n";
        }
        return 0;
    } catch (const std::bad_alloc&) {
        std::cerr << "error: insufficient memory\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
