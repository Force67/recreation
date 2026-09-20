// server_consoletest: the dedicated server's console command table. Everything
// the console can do to a server goes through the ConsoleHost sinks, so the
// whole table runs here against recorders: the replies, the tokenizer's handling
// of stray spacing, the parses that must be refused rather than acted on, a
// convar actually taking a value, and the rule that an unknown command belongs
// to the mods unless there are none to ask.

#include <base/option.h>

#include <cstdio>

#include "runtime/app/server_console.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

// A convar of the test's own, so setting it cannot disturb a real one.
base::Option<int> TestKnob{"test.console.knob", 7, nullptr, "a knob to turn"};

// One console run: the lines it printed and what it did to the server.
struct Run {
  base::Vector<base::String> out;
  bool quit = false;
  bool reloaded = false;
  f32 hour = -1.0f;
  rx::u64 weather = 0;
  base::String forwarded;

  bool Said(const char* needle) const {
    for (const base::String& line : out)
      if (line.find(needle) != base::String::npos)
        return true;
    return false;
  }
};

// A host with every sink wired, recording into `run`. `forwards` is what the
// managed world would answer: false stands for "there is no managed world".
rx::ConsoleHost HostFor(Run& run, bool forwards = true) {
  rx::ConsoleHost host;
  host.server_name = base::String("Test Server");
  host.mods_dir = base::String("/srv/mods");
  host.port = 29700;
  host.max_clients = 32;
  host.player_count = []() { return rx::u32{3}; };
  host.uptime_seconds = []() { return 3725.0; };  // 1h 2m 5s
  host.game_hour = []() { return 13.5f; };
  host.quit = [&run]() { run.quit = true; };
  host.reload_mods = [&run]() { run.reloaded = true; };
  host.set_time = [&run](f32 hour) { run.hour = hour; };
  host.set_weather = [&run](rx::u64 form) { run.weather = form; };
  host.weathers = []() {
    base::Vector<base::Pair<rx::u64, base::String>> pool;
    pool.push_back({0x10E1Full, base::String("SkyrimClear")});
    pool.push_back({0x10E1Aull, base::String("SkyrimOvercast")});
    pool.push_back({0x10E20ull, base::String("SkyrimStormRain")});
    return pool;
  };
  host.forward = [&run, forwards](const base::String& line) {
    run.forwarded = line;
    return forwards;
  };
  return host;
}

// Runs one line against a fresh recorder.
Run RunLine(const char* line, bool forwards = true) {
  Run run;
  const rx::ConsoleHost host = HostFor(run, forwards);
  rx::RunConsoleLine(host, base::String(line),
                     [&run](const base::String& text) { run.out.push_back(text); });
  return run;
}

}  // namespace

int main() {
  std::printf("server_consoletest\n");

  // Nothing to run: blank lines and comments are not unknown commands.
  Check("a blank line does nothing", RunLine("").out.empty());
  Check("spaces alone do nothing", RunLine("   ").out.empty());
  Check("a comment does nothing", RunLine("# restart at midnight").out.empty());

  // help lists the table.
  const Run help = RunLine("help");
  Check("help lists the commands", help.Said("status") && help.Said("convars") &&
                                       help.Said("weather") && help.Said("quit"));

  // status reports what an operator wants to know at a glance.
  const Run status = RunLine("status");
  Check("status names the server", status.Said("Test Server"));
  Check("status counts the players", status.Said("3/32"));
  Check("status gives the port", status.Said("29700"));
  Check("status formats the uptime", status.Said("1h 2m 5s"));
  Check("status shows the game time", status.Said("13:30"));
  Check("status shows the mods directory", status.Said("/srv/mods"));

  // The actions.
  const Run reload = RunLine("reload");
  Check("reload asks for a re-scan", reload.reloaded && reload.Said("re-scanning"));

  const Run quit = RunLine("quit");
  Check("quit stops the server", quit.quit);
  Check("stop is the same command", RunLine("stop").quit);
  Check("exit is the same command", RunLine("exit").quit);

  // Time, in the shapes an operator types it.
  Check("time takes hh:mm", RunLine("time 13:30").hour == 13.5f);
  Check("time takes a fraction", RunLine("time 13.5").hour == 13.5f);
  Check("time takes a whole hour", RunLine("time 7").hour == 7.0f);
  Check("extra spacing does not matter", RunLine("  time    7  ").hour == 7.0f);
  const Run bad_hour = RunLine("time 25");
  Check("an hour out of the day is refused", bad_hour.hour < 0.0f && bad_hour.Said("usage"));
  Check("a nonsense hour is refused", RunLine("time later").hour < 0.0f);
  Check("bad minutes are refused", RunLine("time 13:99").hour < 0.0f);
  Check("time with no argument is refused", RunLine("time").hour < 0.0f);

  // Weather, which an operator types by name rather than by form id.
  Check("weather takes a hex form", RunLine("weather 10E1F").weather == 0x10E1Full);
  Check("weather takes an editor id", RunLine("weather SkyrimOvercast").weather == 0x10E1Aull);
  Check("the name is matched case-insensitively",
        RunLine("weather skyrimovercast").weather == 0x10E1Aull);
  Check("a unique part of a name is enough",
        RunLine("weather StormRain").weather == 0x10E20ull);
  const Run ambiguous = RunLine("weather Skyrim");
  Check("an ambiguous name lists the matches instead of guessing",
        ambiguous.weather == 0 && ambiguous.Said("matches 3 weathers") &&
            ambiguous.Said("SkyrimClear"));
  const Run unknown_weather = RunLine("weather sunny");
  Check("an unknown name says so",
        unknown_weather.weather == 0 && unknown_weather.Said("no weather called"));
  const Run listed_weather = RunLine("weather");
  Check("weather alone lists what there is",
        listed_weather.weather == 0 && listed_weather.Said("weathers (3)") &&
            listed_weather.Said("SkyrimStormRain"));

  // Convars: a real one, turned through the console.
  TestKnob.set(7);
  const Run set = RunLine("set test.console.knob 42");
  Check("set turns the knob", TestKnob.get() == 42);
  Check("set reports the new value", set.Said("test.console.knob = 42"));
  Check("an unknown convar says so", RunLine("set no.such.knob 1").Said("no convar"));
  Check("set needs a value", RunLine("set test.console.knob").Said("usage"));
  const Run listed = RunLine("convars test.console");
  Check("convars lists a match", listed.Said("test.console.knob"));
  Check("convars carries the description", listed.Said("a knob to turn"));
  Check("convars filters out the rest", !listed.Said("net.bubble"));
  Check("a filter matching nothing says so", RunLine("convars zzzz.nothing").Said("no convar"));

  // Anything else belongs to the mods.
  const Run forwarded = RunLine("kick 3 spamming");
  Check("an unknown command goes to the mods", forwarded.forwarded == "kick 3 spamming");
  Check("and the console stays quiet about it", forwarded.out.empty());

  // ...unless there are none to ask, which is the one case the console answers.
  const Run nobody = RunLine("kick 3", /*forwards=*/false);
  Check("with no mods to ask, the console says unknown", nobody.Said("unknown command: kick"));

  std::printf("server_consoletest: %d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
