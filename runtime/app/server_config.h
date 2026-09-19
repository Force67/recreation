#ifndef RECREATION_RUNTIME_APP_SERVER_CONFIG_H_
#define RECREATION_RUNTIME_APP_SERVER_CONFIG_H_

#include <base/containers/pair.h>
#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "core/types.h"

namespace rx {

// What a server config file said. A server is configured by a file of plain
// lines rather than a wall of flags, because a server outlives the shell that
// started it and its settings want to live somewhere an operator can read, diff
// and keep:
//
//   # my server
//   name       Skyrim Together
//   port       29700
//   data_dir   /games/Skyrim Special Edition/Data
//   mods_dir   ./server_mods
//   set        net.bubble.radius 96
//   say        the server is up
//
// The file is parsed into three kinds of thing, because they apply at three
// different moments: startup settings the host needs before it opens a socket,
// convars subsystems read while coming up, and console lines that only mean
// anything once the world is there.
struct ServerConfig {
  // Startup settings, already in command-line form ("--port", "29700", ...) so
  // the one place that maps a setting onto the engine config stays the argument
  // parser. A real command line is applied after these, and so wins.
  base::Vector<base::String> args;
  // `set <name> <value>` lines, applied before bring-up: a convar a subsystem
  // reads as it starts (the bubble radius, say) is too late by the first frame.
  base::Vector<base::Pair<base::String, base::String>> convars;
  // Everything else, run through the server console once the world is up.
  base::Vector<base::String> console_lines;
  // Lines that named a setting with nothing to set it to. Reported, not fatal:
  // a typo in one line should not keep a server from starting.
  base::Vector<base::String> errors;
};

// Parses the text of a config file. Blank lines and `#` comments are ignored;
// a setting is `<key> <value...>`, where the value runs to the end of the line
// so a path or a server name need no quoting.
ServerConfig ParseServerConfig(const base::String& text);

// Reads and parses the file at `path`. `*out_found` reports whether the file
// existed at all, so a caller can tell an empty config from a missing one.
ServerConfig LoadServerConfig(const base::String& path, bool* out_found);

}  // namespace rx

#endif  // RECREATION_RUNTIME_APP_SERVER_CONFIG_H_
