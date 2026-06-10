#ifndef RECREATION_CORE_LOG_H_
#define RECREATION_CORE_LOG_H_

#include <format>
#include <string_view>

namespace rec {

enum class LogLevel { kTrace, kDebug, kInfo, kWarn, kError };

namespace detail {
void LogMessage(LogLevel level, std::string_view message);
}

void SetLogLevel(LogLevel level);

template <typename... Args>
void Log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
  detail::LogMessage(level, std::format(fmt, std::forward<Args>(args)...));
}

#define REC_TRACE(...) ::rec::Log(::rec::LogLevel::kTrace, __VA_ARGS__)
#define REC_DEBUG(...) ::rec::Log(::rec::LogLevel::kDebug, __VA_ARGS__)
#define REC_INFO(...) ::rec::Log(::rec::LogLevel::kInfo, __VA_ARGS__)
#define REC_WARN(...) ::rec::Log(::rec::LogLevel::kWarn, __VA_ARGS__)
#define REC_ERROR(...) ::rec::Log(::rec::LogLevel::kError, __VA_ARGS__)

}  // namespace rec

#endif  // RECREATION_CORE_LOG_H_
