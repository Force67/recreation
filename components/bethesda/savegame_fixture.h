#ifndef COMPONENTS_BETHESDA_SAVEGAME_FIXTURE_H
#define COMPONENTS_BETHESDA_SAVEGAME_FIXTURE_H

// Synthetic savegames, byte-exact to the real layouts, for tests that must run
// without a copy of anyone's Skyrim install.
//
// The savegame reader parses a file the player supplies, so it is the one part
// of this codebase that has to hold up against bytes chosen to break it. The
// tests that read a real .ess prove the reader understands a real save; they
// cannot run in CI, because the save is 14 MB of someone's licensed game data.
// These builders cover the other half: a known-good file the checks can assert
// exact field values against, and a seed the fuzzer mutates.
//
// Field values are arbitrary. The layout is not: offsets in the file location
// table are file-absolute, so a build has to survive the same arithmetic a real
// save does or the reader will disagree with it for the wrong reason.

#include <base/containers/span.h>
#include <base/containers/vector.h>

#include "core/types.h"

namespace rx::bethesda {

// Skyrim LE ("TESV_SAVEGAME"): RGB screenshot, uncompressed body, two change
// forms of which one is zlib compressed, three form ids, three globals.
base::Vector<u8> BuildSyntheticSkyrimLeSave();

// Fallout 4 ("FO4_SAVEGAME"): the later header shape and its own record ids.
base::Vector<u8> BuildSyntheticFallout4Save();

// A valid Papyrus heap table (the layer-2 script/instance/variable store): two
// scripts, one inheriting the other, with instances and an array. The fuzzer
// mutates this rather than assembling bytes blind, so its cases actually reach
// the flattener and the instance walk instead of being rejected at the header.
base::Vector<u8> BuildSyntheticPapyrusHeap();

// The form ids BuildSyntheticSkyrimLeSave writes into its form id array, in
// order. A RefId of kind 0 indexes this one-based.
base::Span<const u32> SyntheticSkyrimFormIds();

}  // namespace rx::bethesda

#endif  // COMPONENTS_BETHESDA_SAVEGAME_FIXTURE_H
