#ifndef RECREATION_RUNTIME_ENGINE_INTERNAL_H_
#define RECREATION_RUNTIME_ENGINE_INTERNAL_H_

// Helpers shared between the Engine translation units (engine*.cc). Kept inline
// in a header so each unit that needs them gets one definition.

#include <cstdarg>
#include <cstdio>
#include <string>

#include "bethesda/game_profile.h"
#include "core/math.h"
#include "world/components.h"

namespace rx {

// A stable per-game slug stored in the editor's layout file, so a saved
// placement reloads against the same game next run. Shared by the content-load
// and main-menu Engine translation units.
inline std::string GameSlug(bethesda::Game game) {
  switch (game) {
    case bethesda::Game::kSkyrimSe:
      return "skyrimse";
    case bethesda::Game::kFallout4:
      return "fallout4";
    case bethesda::Game::kFallout76:
      return "fallout76";
    case bethesda::Game::kStarfield:
      return "starfield";
    default:
      return "game";
  }
}

// printf into a std::string. Attributed so the compiler still type-checks the
// format at each call site (GCC/Clang only; MSVC has no equivalent here).
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline std::string Fmt(const char* fmt, ...) {
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

}  // namespace rx

#endif  // RECREATION_RUNTIME_ENGINE_INTERNAL_H_
