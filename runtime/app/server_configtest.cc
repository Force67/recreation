// server_configtest: the dedicated server's config file. A file of plain lines
// has to split three ways -- the startup settings the host needs before it opens
// a socket, the convars subsystems read as they come up, and the commands that
// only mean anything once the world is there -- so this covers that split, the
// spacing and comments an operator will actually write, and the lines that are
// wrong in a way worth reporting.

#include <cstdio>

#include "runtime/app/server_config.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

bool HasArgPair(const rx::ServerConfig& config, const char* flag, const char* value) {
  for (size_t i = 0; i + 1 < config.args.size(); ++i)
    if (config.args[i] == flag && config.args[i + 1] == value)
      return true;
  return false;
}

bool HasArg(const rx::ServerConfig& config, const char* flag) {
  for (const base::String& arg : config.args)
    if (arg == flag)
      return true;
  return false;
}

bool HasConvar(const rx::ServerConfig& config, const char* name, const char* value) {
  for (const auto& convar : config.convars)
    if (convar.first == name && convar.second == value)
      return true;
  return false;
}

}  // namespace

int main() {
  std::printf("server_configtest\n");

  // A config an operator would plausibly write, spacing and all.
  const rx::ServerConfig config = rx::ParseServerConfig(base::String(
      "# my server\n"
      "\n"
      "name        Skyrim Together\n"
      "port        29700\n"
      "max_clients 48\n"
      "data_dir    /games/Skyrim Special Edition/Data\n"
      "mods_dir    ./server_mods\n"
      "masterlist  https://list.example/api\n"
      "private\n"
      "game        skyrimse\n"
      "plugins     /games/plugins.txt\n"
      "\n"
      "set net.bubble.radius 96\n"
      "set net.quest.log 1\n"
      "\n"
      "time 08:00\n"
      "say the server is up\n"));

  Check("a name keeps its spaces", HasArgPair(config, "--server-name", "Skyrim Together"));
  Check("the port becomes its flag", HasArgPair(config, "--port", "29700"));
  Check("max_clients becomes its flag", HasArgPair(config, "--max-clients", "48"));
  Check("a path with a space survives",
        HasArgPair(config, "--data-dir", "/games/Skyrim Special Edition/Data"));
  Check("the mods directory becomes its flag", HasArgPair(config, "--mods-dir", "./server_mods"));
  Check("the masterlist becomes its flag",
        HasArgPair(config, "--masterlist", "https://list.example/api"));
  Check("the game becomes its flag", HasArgPair(config, "--game", "skyrimse"));
  Check("plugins becomes its flag", HasArgPair(config, "--plugins", "/games/plugins.txt"));
  Check("a valueless setting becomes a bare flag", HasArg(config, "--private"));

  Check("a convar is held apart from the settings",
        HasConvar(config, "net.bubble.radius", "96") && HasConvar(config, "net.quest.log", "1"));
  Check("convars do not leak into the arguments", !HasArg(config, "--set"));

  Check("two console lines were left for the console", config.console_lines.size() == 2);
  Check("a command keeps its whole line",
        config.console_lines.size() == 2 && config.console_lines[0] == "time 08:00" &&
            config.console_lines[1] == "say the server is up");
  Check("comments and blank lines leave nothing behind", config.errors.empty());

  // Whitespace an operator leaves behind.
  const rx::ServerConfig padded =
      rx::ParseServerConfig(base::String("   port   29800   \n\t\n   # trailing comment\n"));
  Check("leading and trailing spaces are trimmed", HasArgPair(padded, "--port", "29800"));
  Check("a tab-only line is nothing", padded.console_lines.empty() && padded.errors.empty());

  // A setting with nothing to set.
  const rx::ServerConfig broken =
      rx::ParseServerConfig(base::String("port\nset\nset lonely\nname\n"));
  Check("a setting with no value is an error", broken.errors.size() == 4);
  Check("and it does not become an argument", broken.args.empty());
  Check("and an incomplete set is not a convar", broken.convars.empty());

  // An empty file is a valid config that says nothing.
  const rx::ServerConfig empty = rx::ParseServerConfig(base::String(""));
  Check("an empty config says nothing", empty.args.empty() && empty.convars.empty() &&
                                            empty.console_lines.empty() && empty.errors.empty());

  // A missing file is reported as missing, not as empty.
  bool found = true;
  rx::LoadServerConfig(base::String("/nonexistent/recreation/server.cfg"), &found);
  Check("a missing file is reported missing", !found);

  std::printf("server_configtest: %d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
