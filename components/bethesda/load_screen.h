#ifndef RECREATION_COMPONENTS_BETHESDA_LOAD_SCREEN_H_
#define RECREATION_COMPONENTS_BETHESDA_LOAD_SCREEN_H_

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "components/bethesda/load_order.h"
#include "components/bethesda/record.h"

// The games' own loading screens, read from their LSCR records.
//
// Every Bethesda title since Oblivion shows a single object turning in the dark
// while it loads, with a line of text about it, and it ships the whole
// presentation as data: which object, how big, which way up, and what to say.
// Skyrim authors 351 of them. Reading those records is how a loading screen
// here can show the same relics, weapons and creatures the original does
// without inventing art or copying any -- the same bargain the rest of the
// engine makes with the games it loads.
namespace rx::bethesda {

struct LoadScreen {
  GlobalFormId id{0xffff, 0};
  // NNAM: the object whose model is displayed. Usually a STAT, occasionally
  // another model-bearing type; the caller resolves it to a mesh path.
  GlobalFormId model{0xffff, 0};
  u32 description = 0;          // DESC: localized string id, the blurb
  f32 scale = 1.0f;             // SNAM
  i16 rotation[3] = {0, 0, 0};  // RNAM: initial rotation, degrees
  f32 offset[3] = {0, 0, 0};    // XNAM: translation, game units
};

// Every LSCR that names a model, in record order. Returns the count.
//
// CTDA conditions are deliberately ignored. They exist to gate which screens a
// given playthrough may show (has the player been to Riften, is this quest
// running) and there is no save loaded when a loading screen is chosen, so
// honouring them would leave only the unconditional handful.
int LoadLoadScreens(const RecordStore& records, base::Vector<LoadScreen>* out);

// The mesh path for a load screen's model ("meshes/..." as the asset database
// wants it), or empty when the reference resolves to nothing displayable.
base::String LoadScreenModelPath(const RecordStore& records, const LoadScreen& screen);

}  // namespace rx::bethesda

#endif  // RECREATION_COMPONENTS_BETHESDA_LOAD_SCREEN_H_
