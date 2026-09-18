#include "runtime/app/script_trust.h"

#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/strings/to_string.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace rx {
namespace {

namespace fs = std::filesystem;

// The per-platform user config directory; mirrors first_run.cc's SetupDir so
// the trust file lives beside the setup.ini the player already has.
fs::path SetupDir() {
#if defined(_WIN32)
  if (const char* a = std::getenv("APPDATA"))
    return fs::path(a) / "Recreation";
  return fs::path("Recreation");
#elif defined(__APPLE__)
  if (const char* h = std::getenv("HOME"))
    return fs::path(h) / "Library" / "Application Support" / "Recreation";
  return fs::path("Recreation");
#else
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x)
    return fs::path(x) / "recreation";
  if (const char* h = std::getenv("HOME"))
    return fs::path(h) / ".config" / "recreation";
  return fs::path(".recreation");
#endif
}

fs::path TrustFile() {
  return SetupDir() / "script_trust.ini";
}

// Reads the file as an ordered list of key/decision lines. A missing file is an
// empty list; a malformed line is skipped, never fatal.
struct TrustLine {
  base::String key;
  base::String decision;
};

base::Vector<TrustLine> ReadDecisions() {
  base::Vector<TrustLine> lines;
  std::ifstream in(TrustFile().c_str(), std::ios::binary);
  if (!in)
    return lines;
  std::ostringstream buffer;
  buffer << in.rdbuf();
  std::istringstream source(buffer.str());
  std::string raw;
  while (std::getline(source, raw)) {
    const auto eq = raw.find('=');
    if (eq == std::string::npos || eq == 0)
      continue;
    TrustLine line;
    line.key = base::String(raw.c_str(), eq);
    line.decision = base::String(raw.c_str() + eq + 1, raw.size() - eq - 1);
    lines.push_back(base::move(line));
  }
  return lines;
}

}  // namespace

base::String ScriptTrust::KeyFor(const base::String& connect_address) {
  // Hosts are case-insensitive, so one line per server regardless of how the
  // player typed the address into the join screen.
  base::String key = connect_address;
  for (char& c : key)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return key;
}

ScriptTrust::Decision ScriptTrust::DecisionFor(const base::String& server_key) {
  for (const TrustLine& line : ReadDecisions()) {
    if (line.key == server_key) {
      if (line.decision == "always")
        return Decision::kAlways;
      if (line.decision == "never")
        return Decision::kNever;
      return Decision::kAsk;
    }
  }
  return Decision::kAsk;
}

void ScriptTrust::Remember(const base::String& server_key, bool allow) {
  std::error_code ec;
  fs::create_directories(SetupDir(), ec);
  const base::String decision = allow ? base::String("always") : base::String("never");
  base::Vector<TrustLine> lines = ReadDecisions();
  bool replaced = false;
  for (TrustLine& line : lines) {
    if (line.key == server_key) {
      line.decision = decision;
      replaced = true;
    }
  }
  if (!replaced) {
    TrustLine line;
    line.key = server_key;
    line.decision = decision;
    lines.push_back(base::move(line));
  }
  std::ofstream out(TrustFile().c_str(), std::ios::binary | std::ios::trunc);
  if (!out)
    return;  // best effort: a read-only config dir must not break the join
  for (const TrustLine& line : lines)
    out << line.key.c_str() << '=' << line.decision.c_str() << '\n';
}

}  // namespace rx
