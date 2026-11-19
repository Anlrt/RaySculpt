#include "image_file.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace caustic {
namespace {

constexpr std::size_t max_image_pixels = 16 * 1024 * 1024;

std::size_t image_size(unsigned width, unsigned height) {
    if (width == 0 || height == 0 || height > max_image_pixels / width)
        throw std::invalid_argument("image must contain between 1 and 16777216 pixels");
    return static_cast<std::size_t>(width) * height;
}

int pgm_number(std::istream& in) {
    while (true) {
        in >> std::ws;
        if (in.peek() != '#') break;
        in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    std::array<char, 12> digits{};
    std::size_t size = 0;
    while (in.peek() >= '0' && in.peek() <= '9' && size < digits.size())
        digits[size++] = static_cast<char>(in.get());
    int value = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + size, value);
    if (parsed.ec != std::errc{} || value <= 0 ||
        (in.peek() != '#' && !std::isspace(in.peek())))
        throw std::invalid_argument("invalid binary PGM header");
    return value;
}

LinearImage load_pgm(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (in.get() != 'P' || in.get() != '5' || !std::isspace(in.peek()))
        throw std::invalid_argument("PGM target must use binary P5");
    const int width = pgm_number(in);
    const int height = pgm_number(in);
    const int maximum = pgm_number(in);
    const int separator = in.get();
    if (maximum > 65535 || !std::isspace(separator))
        throw std::invalid_argument("invalid PGM sample range or raster separator");
    if (separator == '\r' && in.peek() == '\n') in.get();
    LinearImage result{width, height, std::vector<double>(image_size(width, height))};
    for (double& pixel : result.pixels) {
        int value = in.get();
        if (maximum > 255 && value != EOF) {
            const int low = in.get();
            value = low == EOF ? EOF : (value << 8) | low;
        }
        if (value == EOF || value > maximum) throw std::invalid_argument("invalid PGM raster");
        pixel = static_cast<double>(value) / maximum;
    }
    return result;
}

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

void check(HRESULT result, const char* message) {
    if (FAILED(result)) throw std::runtime_error(message);
}

class ComApartment {
public:
    ComApartment() {
        const auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result != RPC_E_CHANGED_MODE) check(result, "cannot initialize Windows image I/O");
        owns_initialization_ = SUCCEEDED(result);
    }
    ~ComApartment() { if (owns_initialization_) CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
private:
    bool owns_initialization_{};
};

ComPtr<IWICImagingFactory> imaging_factory() {
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(factory.GetAddressOf())), "cannot open Windows image codecs");
    return factory;
}

LinearImage load_windows_image(const std::filesystem::path& path) {
    const ComApartment apartment;
    const auto factory = imaging_factory();
    ComPtr<IWICBitmapDecoder> decoder;
    check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand, &decoder),
          "cannot decode target image (use PNG, JPEG, BMP or PGM)");
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame), "target has no readable image frame");
    UINT width = 0, height = 0;
    check(frame->GetSize(&width, &height), "cannot read target dimensions");
    const auto size = image_size(width, height);
    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(&converter), "cannot create target color converter");
    check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0, WICBitmapPaletteTypeCustom),
          "cannot convert target colors");
    std::vector<std::uint8_t> bgra(size * 4);
    check(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(bgra.size()), bgra.data()),
          "cannot read target pixels");
    std::array<double, 256> linear{};
    for (std::size_t i = 0; i < linear.size(); ++i) {
        const double value = static_cast<double>(i) / 255;
        linear[i] = value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    }
    LinearImage result{static_cast<int>(width), static_cast<int>(height), std::vector<double>(size)};
    for (std::size_t i = 0; i < size; ++i) {
        const auto* p = &bgra[i * 4];
        result.pixels[i] = (0.2126 * linear[p[2]] + 0.7152 * linear[p[1]] + 0.0722 * linear[p[0]])
                         * (static_cast<double>(p[3]) / 255);
    }
    return result;
}
#endif

} // namespace

LinearImage load_target_image(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (ext == ".pgm") return load_pgm(path);
#ifdef _WIN32
    return load_windows_image(path);
#else
    throw std::invalid_argument("this build supports only PGM targets");
#endif
}

LinearImage resize_target(const LinearImage& image, int width, int height) {
    LinearImage result{width, height, std::vector<double>(image_size(width, height))};
    const double sx = static_cast<double>(image.width) / width;
    const double sy = static_cast<double>(image.height) / height;
    for (int y = 0; y < height; ++y) {
        const double top = y * sy;
        const double bottom = std::min((y + 1) * sy, static_cast<double>(image.height));
        for (int x = 0; x < width; ++x) {
            const double left = x * sx;
            const double right = std::min((x + 1) * sx, static_cast<double>(image.width));
            double sum = 0;
            for (int j = static_cast<int>(top); j < bottom; ++j) {
                const double wy = std::min(bottom, j + 1.0) - std::max(top, static_cast<double>(j));
                for (int i = static_cast<int>(left); i < right; ++i) {
                    const double wx = std::min(right, i + 1.0) - std::max(left, static_cast<double>(i));
                    sum += wx * wy * image.pixels[static_cast<std::size_t>(j) * image.width + i];
                }
            }
            result.pixels[static_cast<std::size_t>(y) * width + x] = sum / (sx * sy);
        }
    }
    return result;
}

void save_projection_image(const std::filesystem::path& path, int width, int height,
                           std::span<const double> pixels, double exposure) {
    if (pixels.size() != image_size(width, height)) throw std::invalid_argument("invalid preview size");
    std::vector<std::uint8_t> samples;
    samples.reserve(pixels.size());
    for (double value : pixels) {
        const double linear = std::clamp(value * exposure, 0.0, 1.0);
#ifdef _WIN32
        const double encoded = linear <= 0.0031308 ? 12.92 * linear
                                                   : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
#else
        const double encoded = linear;
#endif
        samples.push_back(static_cast<std::uint8_t>(std::lround(encoded * 255)));
    }
#ifdef _WIN32
    const ComApartment apartment;
    const auto factory = imaging_factory();
    ComPtr<IWICStream> stream;
    check(factory->CreateStream(&stream), "cannot create PNG stream");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "cannot open PNG output");
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "cannot create PNG encoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "cannot initialize PNG output");
    ComPtr<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(&frame, nullptr), "cannot create PNG frame");
    check(frame->Initialize(nullptr), "cannot initialize PNG frame");
    check(frame->SetSize(width, height), "cannot set PNG dimensions");
    WICPixelFormatGUID format = GUID_WICPixelFormat8bppGray;
    check(frame->SetPixelFormat(&format), "cannot set PNG pixel format");
    if (format != GUID_WICPixelFormat8bppGray) throw std::runtime_error("grayscale PNG unavailable");
    check(frame->WritePixels(height, width, static_cast<UINT>(samples.size()), samples.data()),
          "cannot write PNG pixels");
    check(frame->Commit(), "cannot commit PNG frame");
    check(encoder->Commit(), "cannot commit PNG output");
#else
    std::ofstream out(path, std::ios::binary);
    out << "P5\n" << width << ' ' << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(samples.data()), samples.size());
    out.close();
    if (!out) throw std::runtime_error("cannot write PGM output");
#endif
}

} // namespace caustic
