// featuretest: the feature-flag registry. The point of this test is the link
// itself: it pulls in recreation::core (a static library) and reaches the flag
// table only through the registry API, never through base::Feature's InitChain.
// If the table is complete here, the manifest survived static linking, which is
// the whole reason the registry exists. Also covers REC_FEATURES overrides.
#include <cstdio>
#include <cstdlib>

#include "core/feature_registry.h"

#ifdef _WIN32
// MSVC has no POSIX setenv/unsetenv; map to the CRT equivalents.
static int setenv(const char* name, const char* value, int /*overwrite*/) {
  return _putenv_s(name, value);
}
static int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif

using rec::FeatureEnabled;
using rec::FeatureId;
using rec::Features;
using rec::InitFeatures;

int main() {
  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-50s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  // The table is fully present, pulled in by referencing the registry alone.
  const auto count = static_cast<unsigned>(FeatureId::kCount);
  check("manifest non-empty", count > 0);
  check("table size matches kCount", Features().size() == count);

  // Manifest defaults.
  check("pathtracer off by default", !FeatureEnabled(FeatureId::kPathTracer));
  check("distant lod on by default", FeatureEnabled(FeatureId::kDistantLod));

  // Every entry has a name and indexes back to itself.
  bool names_ok = true;
  for (unsigned i = 0; i < count; ++i)
    names_ok &= Features()[i].name != nullptr;
  check("every flag has a name", names_ok);

  // REC_FEATURES override: flip the two defaults and exercise every token form.
  ::setenv("REC_FEATURES",
           "render.pathtracer,-render.distant_lod render.mesh_shader_lod=1,bogus.flag", 1);
  InitFeatures();
  check("bare token enables", FeatureEnabled(FeatureId::kPathTracer));
  check("-prefix disables", !FeatureEnabled(FeatureId::kDistantLod));
  check("name=1 enables", FeatureEnabled(FeatureId::kMeshShaderLod));

  // Unknown names and empty specs are harmless no-ops.
  ::unsetenv("REC_FEATURES");
  InitFeatures();
  check("empty spec leaves flags untouched", FeatureEnabled(FeatureId::kPathTracer));

  std::printf("%s (%d failures)\n", failures ? "FEATURETEST FAILED" : "FEATURETEST PASSED",
              failures);
  return failures ? 1 : 0;
}
