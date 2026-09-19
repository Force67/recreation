#ifndef RECREATION_RUNTIME_APP_SERVER_CONSOLE_H_
#define RECREATION_RUNTIME_APP_SERVER_CONSOLE_H_

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include <functional>
#include <memory>

#include "core/types.h"

namespace rx {

// The RPC name a console line the engine has no command for travels to the
// managed world under, where the platform's command registry answers it. The
// managed side subscribes to the same name (sdk/Net/ServerConsole.cs).
inline constexpr const char* kConsoleRpcName = "rx:console";

// What the console can reach in a running server. The runtime fills these in
// from the engine; a test fills them in with its own recorders, which is why the
// command table takes this instead of the Engine. A sink left null means the
// command reports that it cannot do that here rather than crashing.
struct ConsoleHost {
  base::String server_name;
  base::String mods_dir;
  u16 port = 0;
  u32 max_clients = 0;
  std::function<u32()> player_count;
  std::function<f64()> uptime_seconds;
  std::function<f32()> game_hour;
  std::function<void()> quit;
  std::function<void()> reload_mods;
  std::function<void(f32 hour)> set_time;
  std::function<void(u64 weather_form)> set_weather;
  // Hands a line the engine has no command for to the managed world, where the
  // platform's own command registry (kick, say, players, a mod's own commands)
  // answers it and prints the reply. False when there is no managed world to
  // ask, which is the only case the console itself calls unknown.
  std::function<bool(const base::String& line)> forward;
};

// Runs one line the operator typed. Every reply goes through `out`, so the
// runtime prints to the terminal and a test collects. Blank lines and comments
// (`# ...`, as in a startup script) do nothing.
void RunConsoleLine(const ConsoleHost& host,
                    const base::String& line,
                    const std::function<void(const base::String&)>& out);

// The dedicated server's console. A reader thread owns stdin -- reading a line
// blocks, and the simulation must not -- and hands whole lines to the main
// thread, which runs them between frames where touching the engine is safe.
//
// The reader cannot be interrupted portably (it sits in a blocking read), so the
// queue it writes to is shared-owned and outlives this object: after Stop the
// thread finds the queue closed and drops whatever it reads next.
class ServerConsole {
 public:
  ServerConsole();
  ~ServerConsole();

  ServerConsole(const ServerConsole&) = delete;
  ServerConsole& operator=(const ServerConsole&) = delete;

  // Starts reading stdin. No-op when stdin is not readable (a service started
  // with it closed), so a daemonized server does not spin on EOF.
  void Start();
  void Stop();

  // Runs every line typed since the last call, in order. Main thread.
  void Drain(const ConsoleHost& host, const std::function<void(const base::String&)>& out);

 private:
  struct Queue;
  std::shared_ptr<Queue> queue_;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_APP_SERVER_CONSOLE_H_
