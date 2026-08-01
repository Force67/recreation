// Compiles the minicoro implementation in its own translation unit (kept off the
// engine's warning flags via set_source_files_properties). fiber.cc includes
// minicoro.h for the API only.
//
// minicoro picks an asm backend on the common arches (x86_64/aarch64/arm) and
// Win32 fibers on Windows; the _XOPEN_SOURCE guard is only for its ucontext
// fallback (some macOS/BSD configs), where the deprecated routines need it.
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif

#define MINICORO_IMPL
#include "minicoro.h"
