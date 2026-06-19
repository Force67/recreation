#ifndef RECREATION_RUNTIME_ENGINE_INTERNAL_H_
#define RECREATION_RUNTIME_ENGINE_INTERNAL_H_

// Helpers shared between the Engine translation units (engine*.cc). Kept inline
// in a header so each unit that needs them gets one definition.

#include <cstdarg>
#include <cstdio>
#include <string>

#include "core/math.h"
#include "world/components.h"

namespace rec {

// printf into a std::string. Attributed so the compiler still type-checks the
// format at each call site.
__attribute__((format(printf, 1, 2))) inline std::string Fmt(const char* fmt, ...) {
  char buf[600];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return buf;
}

inline Mat4 TransformMatrix(const world::Transform& transform) {
  return MakeTranslation({transform.position[0], transform.position[1], transform.position[2]}) *
         MakeFromQuat(transform.rotation[0], transform.rotation[1], transform.rotation[2],
                      transform.rotation[3]) *
         MakeScale(transform.scale);
}

}  // namespace rec

#endif  // RECREATION_RUNTIME_ENGINE_INTERNAL_H_
