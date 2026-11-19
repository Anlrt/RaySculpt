#include "diagnostics.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace caustic {
namespace {

std::string json_string(std::string_view value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    constexpr char digits[] = "0123456789abcdef";
                    out << "\\u00" << digits[ch >> 4] << digits[ch & 0x0f];
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    out << '"';
    return out.str();
}

std::string utc_now() {
    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

const char* severity_name(DiagnosticSeverity severity) {
    switch (severity) {
        case DiagnosticSeverity::Info: return "info";
        case DiagnosticSeverity::Warning: return "warning";
        case DiagnosticSeverity::Error: return "error";
    }
    return "unknown";
}

const char* stop_name(TraceStopReason reason) {
    switch (reason) {
        case TraceStopReason::Completed: return "completed";
        case TraceStopReason::Cancelled: return "cancelled";
        case TraceStopReason::TimedOut: return "timed_out";
    }
    return "unknown";
}

bool open_log(const std::filesystem::path& path, std::ofstream& out, std::string& error) {
    try {
        const auto parent = path.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        out.open(path, std::ios::app | std::ios::binary);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    if (!out) {
        error = "cannot open diagnostic log";
        return false;
    }
    error.clear();
    return true;
}

} // namespace

bool append_preflight_log(const std::filesystem::path& path,
                          const PreflightReport& report,
                          std::uint64_t scene_revision,
                          std::string& error) {
    std::ofstream out;
    if (!open_log(path, out, error)) return false;
    const std::string timestamp = utc_now();
    for (const Diagnostic& d : report.diagnostics) {
        out << "{\"timestamp\":" << json_string(timestamp)
            << ",\"phase\":\"preflight\",\"scene_revision\":" << scene_revision
            << ",\"severity\":" << json_string(severity_name(d.severity))
            << ",\"code\":" << json_string(d.code)
            << ",\"object_id\":" << json_string(d.object_id)
            << ",\"message\":" << json_string(d.message);
        if (d.has_angle) out << ",\"angle_degrees\":" << std::setprecision(17) << d.angle_degrees;
        out << "}\n";
    }
    if (!out) {
        error = "failed to write diagnostic log";
        return false;
    }
    return true;
}

bool append_trace_log(const std::filesystem::path& path,
                      const TraceResult& result,
                      std::uint64_t scene_revision,
                      std::string& error) {
    std::ofstream out;
    if (!open_log(path, out, error)) return false;
    out << "{\"timestamp\":" << json_string(utc_now())
        << ",\"phase\":\"forward_trace\",\"scene_revision\":" << scene_revision
        << ",\"stop_reason\":" << json_string(stop_name(result.stop_reason))
        << ",\"elapsed_seconds\":" << std::setprecision(17) << result.elapsed_seconds
        << ",\"processed\":" << result.stats.processed
        << ",\"source_rejected\":" << result.stats.source_rejected
        << ",\"side_escape\":" << result.stats.side_escape
        << ",\"total_internal_reflection\":" << result.stats.total_internal_reflection
        << ",\"no_receiver\":" << result.stats.no_receiver
        << ",\"receiver_hit\":" << result.stats.receiver_hit
        << ",\"non_finite\":" << result.stats.non_finite
        << "}\n";
    if (!out) {
        error = "failed to write diagnostic log";
        return false;
    }
    return true;
}

} // namespace caustic
