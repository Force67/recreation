#ifndef RECREATION_SWF_FONT_EXPORT_H_
#define RECREATION_SWF_FONT_EXPORT_H_

#include <base/containers/vector.h>
#include <base/strings/string_ref.h>

#include "components/swf/text.h"

namespace rx::swf {

// Writes a SWF font out as a TrueType file.
//
// The games embed their interface typeface in the movies rather than shipping a
// font file, which is why a translated menu otherwise renders in whatever the
// host's default font is. The conversion is close to direct: a SWF glyph is
// already a set of closed quadratic contours, which is exactly what TrueType's
// `glyf` table stores, so only the coordinate system changes (SWF's y grows
// downward, TrueType's grows up) and the units are rescaled to a 2048 em.
//
// Needs a font parsed with outlines (see ParseFont's `with_outlines`). Returns
// an empty vector when the font carries no glyphs.
base::Vector<u8> ExportTrueType(const Font& font, base::StringRef family);

// The family name to give a font: its DefineFontName if it has one, otherwise
// the authored name with Bethesda's leading '$' removed.
base::String FontFamilyName(const Font& font);

}  // namespace rx::swf

#endif  // RECREATION_SWF_FONT_EXPORT_H_
