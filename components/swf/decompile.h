#ifndef RECREATION_SWF_DECOMPILE_H_
#define RECREATION_SWF_DECOMPILE_H_

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "core/types.h"

namespace rx::swf {

// Lifts an AVM1 action block back to ActionScript 2 source.
//
// The stack machine is executed symbolically: every action either builds an
// expression or emits a statement, so the result reads as the code that was
// compiled rather than as a listing. Control flow is recovered for the shapes
// the ActionScript 2 compiler emits (if, if/else, while, do/while); anything
// else degrades to explicit labels and `goto`, which is honest about what the
// bytecode does instead of inventing structure that is not there.
base::String Decompile(ByteSpan code);

// One line per action, with resolved constant-pool references and absolute jump
// targets. Always available, including for blocks the decompiler gives up on.
base::String Disassembly(ByteSpan code);

// Every string the block's constant pools declare, in order. The pool of a
// Bethesda menu script is a readable index of what it touches: member names,
// event names and the game-side variables it binds to.
base::Vector<base::String> ConstantStrings(ByteSpan code);

}  // namespace rx::swf

#endif  // RECREATION_SWF_DECOMPILE_H_
