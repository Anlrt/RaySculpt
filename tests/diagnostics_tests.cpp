#include "diagnostics.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace caustic;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
  try {
    const auto path = std::filesystem::current_path() / "caustic_diagnostics_test.jsonl";
    std::filesystem::remove(path);
    PreflightReport report;
    report.diagnostics.push_back({DiagnosticSeverity::Error, "BAD_GEOMETRY", "screen\"A",
                                  "Overlaps\nsource", 0.0, false});
    std::string error;
    require(append_preflight_log(path, report, 42, error), "preflight log must write");
    TraceResult trace;
    trace.stats.processed = 100;
    trace.stats.receiver_hit = 80;
    require(append_trace_log(path, trace, 42, error), "trace log must write");
    std::ifstream in(path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    require(contents.find("\"scene_revision\":42") != std::string::npos, "revision missing");
    require(contents.find("screen\\\"A") != std::string::npos, "JSON quote not escaped");
    require(contents.find("Overlaps\\nsource") != std::string::npos, "JSON newline not escaped");
    require(contents.find("\"receiver_hit\":80") != std::string::npos, "trace stats missing");
    in.close();
    std::filesystem::remove(path);
    std::cout << "diagnostics tests passed\n";
  } catch (const std::exception& e) {
    std::cerr << "diagnostics tests failed: " << e.what() << '\n';
    return 1;
  }
}
