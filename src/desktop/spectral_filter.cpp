#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstddef>

#ifdef _WIN32
#define RAYSCULPT_EXPORT __declspec(dllexport)
#else
#define RAYSCULPT_EXPORT
#endif

extern "C" RAYSCULPT_EXPORT int raysculpt_spectral_filter_v1(const double *input, const double *symbol,
                                                             double *output, std::size_t rows,
                                                             std::size_t cols, double sigma) noexcept {
    if (!input || !output || input == output || rows == 0 || cols == 0 || rows > 1025 || cols > 1025 ||
        (symbol && (rows % 2 || cols % 2)) || (!symbol && (!std::isfinite(sigma) || sigma <= 0)))
        return 1;
    try {
        const cv::Mat source(int(rows), int(cols), CV_64F, const_cast<double *>(input));
        cv::Mat destination(int(rows), int(cols), CV_64F, output);
        if (symbol) {
            const cv::Mat weights(int(rows), int(cols), CV_64F, const_cast<double *>(symbol));
            cv::Mat spectrum;
            cv::dct(source, spectrum);
            cv::multiply(spectrum, weights, spectrum);
            cv::idct(spectrum, destination);
        } else {
            cv::GaussianBlur(source, destination, cv::Size(), sigma);
        }
    } catch (...) {
        return 2;
    }
    return 0;
}
