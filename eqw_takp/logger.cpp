#include "logger.h"

#include <share.h>
#include <stdarg.h>
#include <stdio.h>

namespace Logger {

static FILE* log_file = nullptr;
static Level log_level = Level::None;

// Wrapper class used to close out log file at static destruction.
class LogFileCloser {
 public:
  LogFileCloser(){};

  ~LogFileCloser() {
    if (log_file) {
      fclose(log_file);
      log_file = nullptr;
    }
  }
};

// Opens a log file if the level != None. Any existing file is truncated.
void Initialize(const char* filename, Level level) {
  log_level = level;
  if (level < Level::Error) return;  // Don't even open a file.

  static LogFileCloser closer;
  log_file = _fsopen(filename, "w", _SH_DENYWR);  // Open in non-exclusive read mode.
}

// Logs at Level::Info filter.
void Info(const char* format, ...) {
  if (!log_file || static_cast<int>(log_level) < static_cast<int>(Level::Info)) return;

  va_list args;
  va_start(args, format);
  vfprintf(log_file, format, args);
  va_end(args);

  fprintf(log_file, "\n");
  fflush(log_file);
}

// Logs at Level::Error filter. Just duplicating since not worth repackaging va_list.
void Error(const char* format, ...) {
  if (!log_file || static_cast<int>(log_level) < static_cast<int>(Level::Error)) return;

  va_list args;
  va_start(args, format);
  vfprintf(log_file, format, args);
  va_end(args);

  fprintf(log_file, "\n");
  fflush(log_file);
}
}  // namespace Logger
