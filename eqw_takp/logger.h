#pragma once

// Extremely simple log support that supports conditional logging.

namespace Logger {

enum class Level { None = 0, Error, Info };

// Opens the log file and enables writing of log outputs. Call once.
void Initialize(const char* filename, Level level = Level::Info);

// Logs at INFO level.
void Info(const char* format, ...);

// Logs at ERROR level.
void Error(const char* format, ...);

}  // namespace Logger