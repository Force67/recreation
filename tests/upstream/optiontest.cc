// optiontest: base::Option<T> + the env adapter. Options self-register like
// base::Feature; InitOptionsFromEnv() walks that list and parses each option's
// env var. Covers the supported types, the default/override/parse-error paths,
// and that a null env field is left alone.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <base/option.h>

#ifdef _WIN32
// MSVC has no POSIX setenv/unsetenv; map to the CRT equivalents.
static int setenv(const char* name, const char* value, int /*overwrite*/) {
  return _putenv_s(name, value);
}
static int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif

// Declared at namespace scope, the way a real subsystem would: a default, plus
// the env var that overrides it. These replace scattered std::getenv checks.
static base::Option<bool> kFlag{"opt.flag", false, "RX_OPT_FLAG"};
static base::Option<int> kCount{"opt.count", 7, "RX_OPT_COUNT"};
static base::Option<float> kScale{"opt.scale", 1.5f, "RX_OPT_SCALE"};
static base::Option<const char*> kName{"opt.name", nullptr, "RX_OPT_NAME"};
static base::Option<int> kNoEnv{"opt.noenv", 42};  // no env var: never touched

int main() {
  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  // Before the adapter runs, every option reads its compiled-in default.
  check("bool default", kFlag.get() == false);
  check("int default", kCount.get() == 7);
  check("float default", kScale.get() == 1.5f);
  check("string default null", kName.get() == nullptr);
  check("not yet overridden", !kCount.overridden());

  ::setenv("RX_OPT_FLAG", "on", 1);
  ::setenv("RX_OPT_COUNT", "128", 1);
  ::setenv("RX_OPT_SCALE", "0.25", 1);
  ::setenv("RX_OPT_NAME", "rainy", 1);
  ::unsetenv("RX_OPT_NOENV");  // even if present, kNoEnv has no env binding

  const auto n = base::InitOptionsFromEnv();

  // Implicit conversion is the intended read path; get() is the explicit one.
  check("bool override", kFlag);
  check("int override", kCount == 128);
  check("float override", kScale.get() == 0.25f);
  check("string override", kName.get() && std::strcmp(kName.get(), "rainy") == 0);
  check("override marked", kCount.overridden());
  check("null-env option untouched", kNoEnv.get() == 42);
  check("override count >= 4", n >= 4);

  // A value that does not parse leaves the option unchanged (still overridden
  // ones stay; bad input is a no-op).
  kCount.set(7);
  ::setenv("RX_OPT_COUNT", "not-a-number", 1);
  base::InitOptionsFromEnv();
  check("bad int parse is a no-op", kCount.get() == 7);

  ::unsetenv("RX_OPT_FLAG");
  ::unsetenv("RX_OPT_COUNT");
  ::unsetenv("RX_OPT_SCALE");
  ::unsetenv("RX_OPT_NAME");

  std::printf("%s (%d failures)\n", failures ? "OPTIONTEST FAILED" : "OPTIONTEST PASSED",
              failures);
  return failures ? 1 : 0;
}
