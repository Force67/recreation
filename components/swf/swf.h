#ifndef RECREATION_SWF_SWF_H_
#define RECREATION_SWF_SWF_H_

#include <base/containers/vector.h>
#include <base/optional.h>
#include <base/strings/string_ref.h>

#include "components/swf/types.h"
#include "core/types.h"

namespace rx::swf {

// Every tag Bethesda's Scaleform menus actually contain, plus the neighbours
// needed to skip cleanly past what they don't. Codes 1000+ are Scaleform's own
// extensions, emitted by the gfxexport tool into .gfx files.
enum class TagCode : u16 {
  kEnd = 0,
  kShowFrame = 1,
  kDefineShape = 2,
  kPlaceObject = 4,
  kRemoveObject = 5,
  kDefineBits = 6,
  kDefineButton = 7,
  kJpegTables = 8,
  kSetBackgroundColor = 9,
  kDefineFont = 10,
  kDefineText = 11,
  kDoAction = 12,
  kDefineFontInfo = 13,
  kDefineSound = 14,
  kStartSound = 15,
  kDefineButtonSound = 17,
  kSoundStreamHead = 18,
  kSoundStreamBlock = 19,
  kDefineBitsLossless = 20,
  kDefineBitsJpeg2 = 21,
  kDefineShape2 = 22,
  kDefineButtonCxform = 23,
  kProtect = 24,
  kPlaceObject2 = 26,
  kRemoveObject2 = 28,
  kDefineShape3 = 32,
  kDefineText2 = 33,
  kDefineButton2 = 34,
  kDefineBitsJpeg3 = 35,
  kDefineBitsLossless2 = 36,
  kDefineEditText = 37,
  kDefineSprite = 39,
  kFrameLabel = 43,
  kSoundStreamHead2 = 45,
  kDefineMorphShape = 46,
  kDefineFont2 = 48,
  kExportAssets = 56,
  kImportAssets = 57,
  kEnableDebugger = 58,
  kDoInitAction = 59,
  kDefineVideoStream = 60,
  kVideoFrame = 61,
  kDefineFontInfo2 = 62,
  kEnableDebugger2 = 64,
  kScriptLimits = 65,
  kSetTabIndex = 66,
  kFileAttributes = 69,
  kPlaceObject3 = 70,
  kImportAssets2 = 71,
  kDefineFontAlignZones = 73,
  kCsmTextSettings = 74,
  kDefineFont3 = 75,
  kSymbolClass = 76,
  kMetadata = 77,
  kDefineScalingGrid = 78,
  kDoAbc = 82,
  kDefineShape4 = 83,
  kDefineMorphShape2 = 84,
  kDefineSceneAndFrameLabelData = 86,
  kDefineBinaryData = 87,
  kDefineFontName = 88,
  kStartSound2 = 89,
  kDefineBitsJpeg4 = 90,
  kDefineFont4 = 91,

  // Scaleform GFx extensions.
  kGfxExporterInfo = 1000,
  kGfxDefineExternalImage = 1001,
  kGfxFontTextureInfo = 1002,
  kGfxDefineExternalGradientImage = 1003,
  kGfxGradient = 1004,
  kGfxDefineGradientMap = 1005,
  kGfxCompactedFont = 1006,
  kGfxDefineExternalSound = 1007,
  kGfxDefineExternalStreamSound = 1008,
  kGfxDefineSubImage = 1009,
  kGfxFontTextureInfoCff = 1010,
  kGfxDefineExternalImage2 = 1011,
};

base::StringRef TagName(u16 code);

struct Tag {
  u16 code = 0;
  ByteSpan body;   // points into SwfFile::body
  u32 offset = 0;  // byte offset of the body inside SwfFile::body, for reports
};

// One opened movie: the header fields plus the decompressed tag stream. `body`
// owns the bytes every Tag::body and every parsed span points into, so the
// SwfFile has to outlive everything decoded from it.
struct SwfFile {
  bool gfx = false;  // Scaleform .gfx container rather than an Adobe .swf
  u8 version = 0;
  Rect frame_size;  // stage bounds in twips
  f32 frame_rate = 0;
  u16 frame_count = 0;
  base::Vector<u8> body;
  base::Vector<Tag> tags;
};

// Accepts FWS/CWS (Adobe, raw and zlib) and FFX/GFX/CFX (Scaleform, raw and
// zlib). Returns an empty optional on a bad signature, a failed inflate or a
// truncated tag stream. LZMA (ZWS) is rejected: no shipped Bethesda menu uses
// it, so the path would be untested guesswork.
base::Optional<SwfFile> OpenSwf(ByteSpan data);

}  // namespace rx::swf

#endif  // RECREATION_SWF_SWF_H_
