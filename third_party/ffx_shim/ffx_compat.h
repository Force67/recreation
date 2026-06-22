// Force-included (-include / /FI) into every FidelityFX SDK source. On every
// platform it pulls in volk before the SDK's <vulkan/vulkan.h> so the backend
// resolves Vulkan entry points through the engine's loader (volk defines
// VK_NO_PROTOTYPES and declares the vk* symbols as loader-populated function
// pointers). On non-Windows it also supplies the MSVC CRT extensions the SDK
// uses unguarded (MSVC provides those natively).
#ifndef RECREATION_FFX_COMPAT_H_
#define RECREATION_FFX_COMPAT_H_
#if defined(__cplusplus)

#include <volk.h>

#if !defined(_WIN32)

#include <cmath>    // ffx_vk.cpp uses floor/log2 without including it
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <locale>   // ffx_vk.cpp uses std::wstring_convert with only <codecvt>

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

// ffx_message.cpp uses this without including ffx_util.h (which defines the
// identical macro).
#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif

inline int wcscpy_s(wchar_t* dst, size_t dst_size, const wchar_t* src) {
  if (!dst || !src || dst_size == 0) return 22;  // EINVAL
  if (std::wcslen(src) + 1 > dst_size) {
    dst[0] = L'\0';
    return 34;  // ERANGE
  }
  std::wcscpy(dst, src);
  return 0;
}

template <size_t N>
int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src) {
  return wcscpy_s(dst, N, src);
}

inline int strcpy_s(char* dst, size_t dst_size, const char* src) {
  if (!dst || !src || dst_size == 0) return 22;
  if (std::strlen(src) + 1 > dst_size) {
    dst[0] = '\0';
    return 34;
  }
  std::strcpy(dst, src);
  return 0;
}

template <size_t N>
int strcpy_s(char (&dst)[N], const char* src) {
  return strcpy_s(dst, N, src);
}

inline int sprintf_s(char* dst, size_t dst_size, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int written = std::vsnprintf(dst, dst_size, fmt, args);
  va_end(args);
  return written;
}

#endif  // !_WIN32
#endif  // __cplusplus
#endif  // RECREATION_FFX_COMPAT_H_
