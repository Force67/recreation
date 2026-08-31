#include "components/swf/swf.h"

#include <base/memory/move.h>

#include <cstring>

#include "components/bethesda/compression.h"

namespace rx::swf {
namespace {

// A tag stream longer than this in a UI movie means the header lied; the
// biggest shipped menu (quest_journal) decompresses to about 1.5 MB.
constexpr u32 kMaxBodySize = 64u * 1024 * 1024;

struct NamedTag {
  u16 code;
  const char* name;
};

constexpr NamedTag kTagNames[] = {
    {0, "End"},
    {1, "ShowFrame"},
    {2, "DefineShape"},
    {4, "PlaceObject"},
    {5, "RemoveObject"},
    {6, "DefineBits"},
    {7, "DefineButton"},
    {8, "JPEGTables"},
    {9, "SetBackgroundColor"},
    {10, "DefineFont"},
    {11, "DefineText"},
    {12, "DoAction"},
    {13, "DefineFontInfo"},
    {14, "DefineSound"},
    {15, "StartSound"},
    {17, "DefineButtonSound"},
    {18, "SoundStreamHead"},
    {19, "SoundStreamBlock"},
    {20, "DefineBitsLossless"},
    {21, "DefineBitsJPEG2"},
    {22, "DefineShape2"},
    {23, "DefineButtonCxform"},
    {24, "Protect"},
    {26, "PlaceObject2"},
    {28, "RemoveObject2"},
    {32, "DefineShape3"},
    {33, "DefineText2"},
    {34, "DefineButton2"},
    {35, "DefineBitsJPEG3"},
    {36, "DefineBitsLossless2"},
    {37, "DefineEditText"},
    {39, "DefineSprite"},
    {43, "FrameLabel"},
    {45, "SoundStreamHead2"},
    {46, "DefineMorphShape"},
    {48, "DefineFont2"},
    {56, "ExportAssets"},
    {57, "ImportAssets"},
    {58, "EnableDebugger"},
    {59, "DoInitAction"},
    {60, "DefineVideoStream"},
    {61, "VideoFrame"},
    {62, "DefineFontInfo2"},
    {64, "EnableDebugger2"},
    {65, "ScriptLimits"},
    {66, "SetTabIndex"},
    {69, "FileAttributes"},
    {70, "PlaceObject3"},
    {71, "ImportAssets2"},
    {73, "DefineFontAlignZones"},
    {74, "CSMTextSettings"},
    {75, "DefineFont3"},
    {76, "SymbolClass"},
    {77, "Metadata"},
    {78, "DefineScalingGrid"},
    {82, "DoABC"},
    {83, "DefineShape4"},
    {84, "DefineMorphShape2"},
    {86, "DefineSceneAndFrameLabelData"},
    {87, "DefineBinaryData"},
    {88, "DefineFontName"},
    {89, "StartSound2"},
    {90, "DefineBitsJPEG4"},
    {91, "DefineFont4"},
    {1000, "GFXExporterInfo"},
    {1001, "GFXDefineExternalImage"},
    {1002, "GFXFontTextureInfo"},
    {1003, "GFXDefineExternalGradientImage"},
    {1004, "GFXGradient"},
    {1005, "GFXDefineGradientMap"},
    {1006, "GFXCompactedFont"},
    {1007, "GFXDefineExternalSound"},
    {1008, "GFXDefineExternalStreamSound"},
    {1009, "GFXDefineSubImage"},
    {1010, "GFXFontTextureInfoCFF"},
    {1011, "GFXDefineExternalImage2"},
};

}  // namespace

base::StringRef TagName(u16 code) {
  for (const NamedTag& t : kTagNames)
    if (t.code == code)
      return t.name;
  return "Unknown";
}

base::Optional<SwfFile> OpenSwf(ByteSpan data) {
  if (data.size() < 8)
    return base::nullopt;

  const char c0 = static_cast<char>(data[0]);
  const char c1 = static_cast<char>(data[1]);
  const char c2 = static_cast<char>(data[2]);

  bool gfx = false;
  bool compressed = false;
  if (c1 == 'W' && c2 == 'S') {
    if (c0 == 'C')
      compressed = true;
    else if (c0 == 'Z')
      return base::nullopt;  // LZMA, see the header
    else if (c0 != 'F')
      return base::nullopt;
  } else if (c1 == 'F' && c2 == 'X') {
    gfx = true;
    if (c0 == 'C')
      compressed = true;
    else if (c0 != 'F' && c0 != 'G')
      return base::nullopt;
  } else {
    return base::nullopt;
  }

  SwfFile file;
  file.gfx = gfx;
  file.version = data[3];

  u32 file_length = 0;
  std::memcpy(&file_length, data.data() + 4, sizeof(file_length));
  if (file_length < 8 || file_length > kMaxBodySize)
    return base::nullopt;

  // file_length counts the 8-byte header that is never compressed.
  const u32 body_size = file_length - 8;
  file.body.resize(body_size);
  const ByteSpan tail = data.subspan(8);
  if (compressed) {
    if (!bethesda::ZlibInflate(tail, file.body.data(), body_size))
      return base::nullopt;
  } else {
    if (tail.size() < body_size)
      return base::nullopt;
    std::memcpy(file.body.data(), tail.data(), body_size);
  }

  const ByteSpan body{file.body.data(), file.body.size()};
  Reader r(body);
  file.frame_size = r.ReadRect();
  file.frame_rate = r.Fixed8();
  file.frame_count = r.U16();
  if (!r.ok())
    return base::nullopt;

  while (r.ok() && !r.eof()) {
    const u16 header = r.U16();
    const u16 code = static_cast<u16>(header >> 6);
    u32 length = header & 0x3fu;
    if (length == 0x3f)
      length = r.U32();
    if (!r.ok())
      break;
    const u32 offset = static_cast<u32>(r.pos());
    const ByteSpan tag_body = r.Bytes(length);
    if (!r.ok())
      break;
    file.tags.push_back(Tag{code, tag_body, offset});
    if (code == static_cast<u16>(TagCode::kEnd))
      break;
  }
  if (file.tags.empty())
    return base::nullopt;

  return base::Optional<SwfFile>(base::move(file));
}

}  // namespace rx::swf
