#include "runtime/app/server_console.h"

#include <base/algorithm.h>
#include <base/option.h>
#include <base/strings/to_string.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "core/log.h"

namespace rx {
namespace {

// "  set  net.bubble.radius 64  " -> ["set", "net.bubble.radius", "64"].
base::Vector<base::String> Tokenize(const base::String& line) {
  base::Vector<base::String> out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && line[i] == ' ')
      ++i;
    const size_t start = i;
    while (i < line.size() && line[i] != ' ')
      ++i;
    if (i > start)
      out.push_back(base::String(line.c_str() + start, i - start));
  }
  return out;
}

// "13:30" / "13.5" / "7" -> hours in [0, 24). Negative on anything else.
f32 ParseHour(const base::String& text) {
  if (text.empty())
    return -1.0f;
  const char* s = text.c_str();
  char* end = nullptr;
  const double hours = std::strtod(s, &end);
  if (end == s)
    return -1.0f;
  double value = hours;
  if (*end == ':') {
    const char* minutes_text = end + 1;
    char* minutes_end = nullptr;
    const double minutes = std::strtod(minutes_text, &minutes_end);
    if (minutes_end == minutes_text || minutes < 0.0 || minutes >= 60.0)
      return -1.0f;
    value = hours + minutes / 60.0;
  }
  if (value < 0.0 || value >= 24.0)
    return -1.0f;
  return static_cast<f32>(value);
}

base::String FormatDuration(f64 seconds) {
  const u64 total = static_cast<u64>(seconds < 0 ? 0 : seconds);
  const u64 h = total / 3600, m = (total / 60) % 60, s = total % 60;
  char buf[48];
  if (h > 0)
    std::snprintf(buf, sizeof(buf), "%lluh %llum %llus", static_cast<unsigned long long>(h),
                  static_cast<unsigned long long>(m), static_cast<unsigned long long>(s));
  else if (m > 0)
    std::snprintf(buf, sizeof(buf), "%llum %llus", static_cast<unsigned long long>(m),
                  static_cast<unsigned long long>(s));
  else
    std::snprintf(buf, sizeof(buf), "%llus", static_cast<unsigned long long>(s));
  return base::String(buf);
}

void PrintHelp(const std::function<void(const base::String&)>& out) {
  out("commands:");
  out("  status              players, uptime and what this server is running");
  out("  reload              re-scan the mods directory and re-offer it to clients");
  out("  time <h[:mm]>       set the time of day for everyone");
  out("  weather <form>      bring in a weather (hex WTHR form id)");
  out("  set <name> <value>  set a convar (see convars)");
  out("  convars [prefix]    list the convars, optionally filtered");
  out("  quit                stop the server");
  out("anything else goes to the server's mods, which answer for their own commands.");
}

void RunStatus(const ConsoleHost& host, const std::function<void(const base::String&)>& out) {
  const u32 players = host.player_count ? host.player_count() : 0;
  base::String line = base::String("name: ") +
                      (host.server_name.empty() ? base::String("(unnamed)") : host.server_name);
  out(line);
  out(base::String("port: ") + base::ToString(static_cast<u64>(host.port)) +
      "   players: " + base::ToString(static_cast<u64>(players)) + "/" +
      base::ToString(static_cast<u64>(host.max_clients)));
  if (host.uptime_seconds)
    out(base::String("up: ") + FormatDuration(host.uptime_seconds()));
  if (host.game_hour) {
    const f32 hour = host.game_hour();
    char clock[16];
    std::snprintf(clock, sizeof(clock), "%02d:%02d", static_cast<int>(hour),
                  static_cast<int>((hour - static_cast<f32>(static_cast<int>(hour))) * 60.0f));
    out(base::String("game time: ") + clock);
  }
  if (!host.mods_dir.empty())
    out(base::String("mods: ") + host.mods_dir);
}

// The convars are a self-registering list, so `set` looks the name up in it.
base::OptionBase* FindConvar(const base::String& name) {
  base::OptionBase* found = nullptr;
  base::OptionBase::VisitAll([&](const base::OptionBase* option) {
    if (!found && name == option->name())
      found = const_cast<base::OptionBase*>(option);
  });
  return found;
}

void RunSet(const base::Vector<base::String>& args,
            const std::function<void(const base::String&)>& out) {
  if (args.size() < 3) {
    out("usage: set <name> <value>");
    return;
  }
  base::OptionBase* option = FindConvar(args[1]);
  if (!option) {
    out(base::String("no convar '") + args[1] + "' (try convars)");
    return;
  }
  if (!option->SetFromString(args[2].c_str())) {
    out(base::String("'") + args[2] + "' is not a value " + args[1] + " can take");
    return;
  }
  out(base::String(args[1]) + " = " + args[2]);
}

void RunConvars(const base::Vector<base::String>& args,
                const std::function<void(const base::String&)>& out) {
  const base::String prefix = args.size() >= 2 ? args[1] : base::String();
  u32 shown = 0;
  base::OptionBase::VisitAll([&](const base::OptionBase* option) {
    const base::String name(option->name());
    if (!prefix.empty() && name.find(prefix) == base::String::npos)
      return;
    ++shown;
    base::String line = base::String("  ") + name;
    if (option->overridden())
      line += " (set)";
    if (option->desc() && *option->desc())
      line += base::String("  -- ") + option->desc();
    out(line);
  });
  if (shown == 0)
    out(base::String("no convar matches '") + prefix + "'");
}

}  // namespace

void RunConsoleLine(const ConsoleHost& host,
                    const base::String& line,
                    const std::function<void(const base::String&)>& out) {
  const base::Vector<base::String> args = Tokenize(line);
  if (args.empty() || args[0][0] == '#')
    return;
  const base::String& name = args[0];

  if (name == "help" || name == "?") {
    PrintHelp(out);
  } else if (name == "status") {
    RunStatus(host, out);
  } else if (name == "reload") {
    if (!host.reload_mods) {
      out("this server has nothing to reload");
      return;
    }
    host.reload_mods();
    out("re-scanning the mods directory");
  } else if (name == "time") {
    const f32 hour = args.size() >= 2 ? ParseHour(args[1]) : -1.0f;
    if (hour < 0.0f) {
      out("usage: time <hour> -- 13:30, 13.5 or 7");
      return;
    }
    if (!host.set_time) {
      out("this server has no clock to set");
      return;
    }
    host.set_time(hour);
    out(base::String("time set to ") + args[1]);
  } else if (name == "weather") {
    if (args.size() < 2) {
      out("usage: weather <form> -- the hex form id of a WTHR record");
      return;
    }
    const u64 form = std::strtoull(args[1].c_str(), nullptr, 16);
    if (form == 0) {
      out(base::String("'") + args[1] + "' is not a form id");
      return;
    }
    if (!host.set_weather) {
      out("this server has no sky to set");
      return;
    }
    host.set_weather(form);
    out(base::String("bringing in weather ") + args[1]);
  } else if (name == "set") {
    RunSet(args, out);
  } else if (name == "convars") {
    RunConvars(args, out);
  } else if (name == "quit" || name == "stop" || name == "exit") {
    if (!host.quit) {
      out("this server cannot stop itself");
      return;
    }
    out("stopping");
    host.quit();
  } else if (!host.forward || !host.forward(line)) {
    // Only reached when there is no managed world to ask: a server with mods
    // answers for its own commands (and for the ones it does not know).
    out(base::String("unknown command: ") + name + " (try help)");
  }
}

struct ServerConsole::Queue {
  std::mutex mutex;
  base::Vector<base::String> lines;
  bool closed = false;
};

ServerConsole::ServerConsole() = default;

ServerConsole::~ServerConsole() {
  Stop();
}

void ServerConsole::Start() {
  if (queue_)
    return;
  // Nothing to read from: a service started with stdin closed would otherwise
  // spin the reader on EOF forever.
  if (std::feof(stdin) || std::ferror(stdin))
    return;
  queue_ = std::make_shared<Queue>();
  // Detached, and holding its own reference to the queue: a blocking read cannot
  // be interrupted portably, so the thread is allowed to outlive this object and
  // finds the queue closed when it next wakes.
  std::thread([queue = queue_]() {
    std::string line;
    while (std::getline(std::cin, line)) {
      std::lock_guard<std::mutex> lock(queue->mutex);
      if (queue->closed)
        return;
      queue->lines.push_back(base::String(line.c_str(), line.size()));
    }
  }).detach();
  RX_INFO("console: type help for commands");
}

void ServerConsole::Stop() {
  if (!queue_)
    return;
  {
    std::lock_guard<std::mutex> lock(queue_->mutex);
    queue_->closed = true;
    queue_->lines.clear();
  }
  queue_ = nullptr;
}

void ServerConsole::Drain(const ConsoleHost& host,
                          const std::function<void(const base::String&)>& out) {
  if (!queue_)
    return;
  base::Vector<base::String> pending;
  {
    std::lock_guard<std::mutex> lock(queue_->mutex);
    pending = base::move(queue_->lines);
    queue_->lines.clear();
  }
  for (const base::String& line : pending)
    RunConsoleLine(host, line, out);
}

}  // namespace rx
