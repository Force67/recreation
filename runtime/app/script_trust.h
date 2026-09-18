#ifndef RECREATION_RUNTIME_APP_SCRIPT_TRUST_H_
#define RECREATION_RUNTIME_APP_SCRIPT_TRUST_H_

#include <base/strings/xstring.h>

namespace rx {

// The player's per-server decisions about running a server's streamed client
// code. Backed by script_trust.ini beside first_run.cc's setup.ini (same
// per-platform user config directory), listing one `host:port=always|never`
// line per server the player answered "always" or "never" on. A server with no
// line is asked every join; the loading screen carries that question.
class ScriptTrust {
 public:
  enum class Decision {
    kAsk,     // no stored decision: show the consent prompt
    kAlways,  // the player allows this server's scripts
    kNever,   // the player declines this server's scripts
  };

  // The trust-store identity of a server: its "host:port" address, normalized.
  static base::String KeyFor(const base::String& connect_address);

  static Decision DecisionFor(const base::String& server_key);

  // Stores a decision, creating or updating the file. Best effort: a failure to
  // persist just means the player is asked again next time.
  static void Remember(const base::String& server_key, bool allow);
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_APP_SCRIPT_TRUST_H_
