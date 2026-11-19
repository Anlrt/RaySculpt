#pragma once

#include "optics.hpp"
#include "preflight.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace caustic {

// Append one JSON record per diagnostic. The caller decides when to log
// interactive checks, so dragging a receiver need not create a record every
// frame. All records carry a scene revision to reject stale background work.
bool append_preflight_log(const std::filesystem::path& path,
                          const PreflightReport& report,
                          std::uint64_t scene_revision,
                          std::string& error);

bool append_trace_log(const std::filesystem::path& path,
                      const TraceResult& result,
                      std::uint64_t scene_revision,
                      std::string& error);

} // namespace caustic
