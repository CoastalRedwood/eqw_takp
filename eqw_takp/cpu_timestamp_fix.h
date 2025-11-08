#pragma once

#include <windows.h>

#include <filesystem>

// Provides optional (ini file setting) support for hooking in a more accurate
// CPU timebase used in the game's primary millisecond timestamp clock. Required
// for high frequency AMD processors.

namespace CpuTimestampFix {

void Initialize(const std::filesystem::path& ini_file);  // Hooks in fix if enabled.

}  // namespace CpuTimestampFix
