#include "core/log.h"

#include <atomic>
#include <cstdio>
#include <mutex>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace rec {
namespace {

std::atomic<LogLevel> g_level{LogLevel::kInfo};
std::mutex g_mutex;

const char* LevelTag(LogLevel level) {
  switch (level) {
    case LogLevel::kTrace: return "trace";
    case LogLevel::kDebug: return "debug";
    case LogLevel::kInfo: return "info";
    case LogLevel::kWarn: return "warn";
    case LogLevel::kError: return "error";
  }
  return "?";
}

}  // namespace

void SetLogLevel(LogLevel level) { g_level.store(level); }

namespace detail {

void LogMessage(LogLevel level, std::string_view message) {
  if (level < g_level.load()) return;
#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_INFO, "recreation", "[%s] %.*s", LevelTag(level),
                      static_cast<int>(message.size()), message.data());
#else
  std::scoped_lock lock(g_mutex);
  std::fprintf(level >= LogLevel::kWarn ? stderr : stdout, "[%s] %.*s\n", LevelTag(level),
               static_cast<int>(message.size()), message.data());
#endif
}

}  // namespace detail
}  // namespace rec
