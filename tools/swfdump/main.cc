#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/memory/unique_pointer.h>
#include <base/strings/format.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "asset/vfs.h"
#include "components/bethesda/archive.h"
#include "components/bethesda/strings.h"
#include "components/swf/abc.h"
#include "components/swf/bridge.h"
#include "components/swf/stage.h"
#include "components/swf/vm.h"
#include "components/swf/decompile.h"
#include "components/swf/font_export.h"
#include "components/swf/movie.h"
#include "components/swf/svg_export.h"
#include "components/swf/swf.h"
#include "components/swf/ugui_export.h"

// Reads the Scaleform movies the Bethesda games ship their whole UI in: the tag
// stream, the vector art, the bitmaps, the text fields, and the ActionScript 2
// bytecode behind all of it. `--ugui` translates a movie into libultragui
// markup plus its assets, which is how the original interface gets rebuilt on
// the engine's own UI stack rather than emulated.
namespace {

using namespace rx;

base::Vector<u8> ReadFile(const char* path) {
  base::Vector<u8> out;
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return out;
  // A directory opens happily and reports LLONG_MAX here, which sizes the
  // vector past any allocator. Anything this large is not a movie either way.
  constexpr std::streamsize kMaxFileSize = 512ll * 1024 * 1024;
  const std::streamsize size = file.tellg();
  if (size <= 0 || size > kMaxFileSize)
    return out;
  file.seekg(0);
  out.resize(static_cast<mem_size>(size));
  file.read(reinterpret_cast<char*>(out.data()), size);
  if (!file)
    out.clear();
  return out;
}

bool WriteFile(const std::filesystem::path& path, const void* data, mem_size size) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary);
  if (!file)
    return false;
  file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  return file.good();
}

bool WriteText(const std::filesystem::path& path, const base::String& text) {
  return WriteFile(path, text.data(), text.size());
}

const char* KindName(swf::CharacterKind kind) {
  switch (kind) {
    case swf::CharacterKind::kShape:
      return "shape";
    case swf::CharacterKind::kBitmap:
      return "bitmap";
    case swf::CharacterKind::kEditText:
      return "edittext";
    case swf::CharacterKind::kStaticText:
      return "text";
    case swf::CharacterKind::kFont:
      return "font";
    case swf::CharacterKind::kSprite:
      return "sprite";
    case swf::CharacterKind::kButton:
      return "button";
    default:
      return "unknown";
  }
}

void PrintSummary(const swf::SwfFile& file, const swf::Movie& movie) {
  std::printf("%s v%u  stage %.0fx%.0f @ %.1f fps, %u frames\n",
              file.gfx ? "gfx" : "swf", file.version,
              static_cast<double>(swf::ToPixels(file.frame_size.width())),
              static_cast<double>(swf::ToPixels(file.frame_size.height())),
              static_cast<double>(file.frame_rate), file.frame_count);
  std::printf("  tags %zu  shapes %zu  bitmaps %zu  edit texts %zu  static texts %zu\n",
              static_cast<size_t>(file.tags.size()),
              static_cast<size_t>(movie.shapes.size()),
              static_cast<size_t>(movie.bitmaps.size()),
              static_cast<size_t>(movie.edit_texts.size()),
              static_cast<size_t>(movie.static_texts.size()));
  if (!movie.external_images.empty())
    std::printf("  external images %zu (the .gfx export keeps its bitmaps in files"
                " beside the movie)\n",
                static_cast<size_t>(movie.external_images.size()));
  std::printf("  fonts %zu  sprites %zu  buttons %zu  exports %zu  scripts %zu\n",
              static_cast<size_t>(movie.fonts.size()),
              static_cast<size_t>(movie.sprites.size()),
              static_cast<size_t>(movie.buttons.size()),
              static_cast<size_t>(movie.exports.size()),
              static_cast<size_t>(movie.scripts.size()));
  if (!movie.abc_blocks.empty()) {
    std::printf("  actionscript 3: %zu DoABC block(s)\n",
                static_cast<size_t>(movie.abc_blocks.size()));
    for (const ByteSpan& block : movie.abc_blocks) {
      swf::AbcFile abc;
      swf::ParseAbc(block, abc);
      for (const swf::ListBinding& b : swf::ParseListBindings(abc))
        std::printf("  list %s.%s -> %u x %s\n", b.owner.c_str(), b.instance.c_str(),
                    b.count, b.entry.c_str());
    }
  }
  for (const base::String& entry : movie.imports)
    std::printf("  imports %s\n", entry.c_str());
}

void PrintTags(const swf::SwfFile& file) {
  for (const swf::Tag& tag : file.tags) {
    const base::StringRef name = swf::TagName(tag.code);
    std::printf("  %6u  %-28.*s %zu bytes\n", tag.code, static_cast<int>(name.size()),
                name.data(), static_cast<size_t>(tag.body.size()));
  }
}

void PrintExports(const swf::Movie& movie) {
  for (const auto& entry : movie.exports) {
    const swf::CharacterRef* ref = movie.characters.find(entry.key);
    std::printf("  %5u  %-9s %s\n", entry.key,
                KindName(ref ? ref->kind : swf::CharacterKind::kUnknown),
                entry.value.c_str());
  }
}

void PrintTexts(const swf::Movie& movie) {
  for (const swf::EditText& text : movie.edit_texts) {
    std::printf("  %5u  %6.1fx%-6.1f %-34s %s\n", text.id,
                static_cast<double>(swf::ToPixels(text.bounds.width())),
                static_cast<double>(swf::ToPixels(text.bounds.height())),
                text.variable.empty() ? "-" : text.variable.c_str(),
                text.initial_text.c_str());
  }
  for (const swf::StaticText& text : movie.static_texts) {
    base::String content;
    for (const swf::TextRun& run : text.runs) {
      if (const swf::Font* font = movie.FindFont(run.font_id))
        content += swf::ResolveRunText(*font, run);
    }
    if (!content.empty())
      std::printf("  %5u  static  %s\n", text.id, content.c_str());
  }
}

// Mounts every archive in a game's Data directory plus its loose files, which
// is where the shipped movies actually live (Skyrim - Interface.bsa,
// Fallout4 - Interface.ba2, ...).
void MountData(asset::Vfs& vfs, const char* data_dir) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec)) {
    if (auto provider = bethesda::OpenArchive(entry.path().string()))
      vfs.Mount(base::move(provider));
  }
  vfs.Mount(asset::MakeLooseFileProvider(data_dir));
}

// Executes a movie's scripts and reports what they built. This is the direct
// test of the interpreter: a shipped menu's init blocks define its classes on
// _global, so the names that turn up there are the ones the game would have.
void RunScripts(const swf::Movie& movie) {
  u32 inits = 0, frames = 0;
  for (const swf::Script& sc : movie.scripts)
    (sc.kind == swf::Script::Kind::kInit ? inits : frames)++;
  std::printf("scripts: %u init, %u frame\n", inits, frames);

  swf::Vm vm;
  swf::GameBridge bridge(vm);
  // The same answers the host gives, registered before anything runs, so this
  // reports the state a player would be shown rather than a quieter one.
  bridge.SetAnswer("ShouldShowMod", swf::AsValue::Bool(false));
  bridge.SetAnswer("ShouldShowKinectTunerOption", swf::AsValue::Bool(false));
  swf::Stage stage(vm, movie);
  stage.set_authored_state(true);
  stage.Run();
  // What the game does once the menu is up: run the extensions that register
  // the rest of its callbacks, then send the message it opens this screen with.
  // Everything reported below is the state the player would have been shown.
  std::printf("%u clip(s) took InitExtensions\n", bridge.Open());
  {
    // sendMenuProperties(canQuit, hasSave, ...): the booleans the game passes
    // for what the build offers. No save, so no CONTINUE.
    base::Vector<swf::AsValue> properties;
    for (int a = 0; a < 15; ++a)
      properties.push_back(swf::AsValue::Bool(a == 0 || a == 8 || a == 9));
    bridge.Invoke("sendMenuProperties", properties);
    // RestoreSavedSettings(tab, tabsDisabled): the journal opened on its System
    // page with the other tabs locked out, which is the pause menu.
    base::Vector<swf::AsValue> tab;
    tab.push_back(swf::AsValue::Number(2));
    tab.push_back(swf::AsValue::Bool(true));
    bridge.Invoke("RestoreSavedSettings", tab);
  }

  // What is actually up. A menu keeps every page it owns in the same tree and
  // hides all but one, so the visible set is the direct measure of whether the
  // screen the player asked for is the screen that would be drawn.
  {
    u32 hidden = 0;
    const u32 objects = vm.object_count();
    for (u32 i = 1; i < objects; ++i) {
      if (!vm.Valid(i) || !vm.Get(i).is_movie_clip)
        continue;
      const swf::AsValue clip = swf::AsValue::Obj(i);
      if (vm.ToBool(vm.GetMember(clip, "_visible")))
        continue;
      ++hidden;
      if (hidden <= 10)
        std::printf("  hidden: %s\n", vm.ToString(vm.GetMember(clip, "_name")).c_str());
    }
    std::printf("%u of %u clip(s) hidden\n", hidden, stage.clip_count());
  }

  // A few frames of it running, the way a host ticks it: a list defers its
  // first layout to an enter-frame, so a snapshot taken before one shows every
  // row still holding its placeholder.
  for (int frame = 0; frame < 8; ++frame)
    stage.Tick(16.0);

  for (u32 i = 1; i < vm.object_count(); ++i) {
    if (!vm.Valid(i) || !vm.Get(i).is_movie_clip) continue;
    const swf::AsValue c = swf::AsValue::Obj(i);
    if (!vm.GetMember(c, "CategoryList").is_object()) continue;
    const swf::AsValue l = vm.GetMember(c, "CategoryList");
    std::printf("DBG page=%s CategoryList=%s entries=%s onLoad=%d state=%s\n",
                vm.ToString(vm.GetMember(c, "_name")).c_str(),
                vm.ToString(vm.GetMember(l, "_name")).c_str(),
                vm.ToString(vm.GetMember(vm.GetMember(l, "entryList"), "length")).c_str(),
                (int)vm.GetMember(c, "onLoad").type(),
                vm.ToString(vm.GetMember(c, "currentState")).c_str());
  }
  for (u32 i = 1; i < vm.object_count(); ++i) {
    if (!vm.Valid(i) || !vm.Get(i).is_movie_clip) continue;
    const swf::AsValue c = swf::AsValue::Obj(i);
    const base::String n = vm.ToString(vm.GetMember(c, "_name"));
    if (n != "QuestsTab" && n != "SystemTab") continue;
    base::String own;
    for (const base::String& k : vm.Get(i).order) { own += k; own += " "; }
    std::printf("DBG %s frame=%s own=[%s]\n", n.c_str(),
                vm.ToString(vm.GetMember(c, "_currentframe")).c_str(), own.c_str());
  }
  // A navigation key through the movie's own components, which is how a host
  // drives every screen's selection without knowing any of them: build the
  // `details` the game builds and hand it to whatever is listening.
  {
    const swf::AsValue details = swf::AsValue::Obj(vm.NewObject());
    vm.SetMember(details, "navEquivalent", swf::AsValue::Str("down"));
    vm.SetMember(details, "value", swf::AsValue::Str("keyDown"));
    const swf::AsValue path = swf::AsValue::Obj(vm.NewArray());
    vm.SetMember(path, "length", swf::AsValue::Number(0));
    u32 took = 0;
    const u32 objects = vm.object_count();
    for (u32 i = 1; i < objects; ++i) {
      if (!vm.Valid(i) || !vm.Get(i).is_movie_clip)
        continue;
      const swf::AsValue clip = swf::AsValue::Obj(i);
      const swf::AsValue entries = vm.GetMember(clip, "entryList");
      if (!entries.is_object() || vm.ToNumber(vm.GetMember(entries, "length")) <= 0)
        continue;
      const f64 before = vm.ToNumber(vm.GetMember(clip, "iSelectedIndex"));
      base::Vector<swf::AsValue> args;
      args.push_back(details);
      args.push_back(path);
      if (!stage.Dispatch(clip, "handleInput", args))
        continue;
      ++took;
      const f64 after = vm.ToNumber(vm.GetMember(clip, "iSelectedIndex"));
      if (took <= 4)
        std::printf("  down on %s: selection %d -> %d\n",
                    vm.ToString(vm.GetMember(clip, "_name")).c_str(),
                    static_cast<int>(before), static_cast<int>(after));
    }
    std::printf("%u list(s) took a navigation key\n", took);
  }

  // Every list the run left with entries in it. A shipped menu ships its lists
  // empty, so this is the direct measure of how much of a screen the movie's
  // own code managed to fill.
  {
    u32 filled = 0;
    const u32 objects = vm.object_count();
    for (u32 i = 1; i < objects; ++i) {
      if (!vm.Valid(i) || !vm.Get(i).is_movie_clip)
        continue;
      const swf::AsValue clip = swf::AsValue::Obj(i);
      const swf::AsValue entries = vm.GetMember(clip, "entryList");
      if (!entries.is_object())
        continue;
      const int n = static_cast<int>(vm.ToNumber(vm.GetMember(entries, "length")));
      if (n <= 0)
        continue;
      ++filled;
      if (filled > 8)
        continue;
      std::printf("  list %s: %d entr%s\n",
                  vm.ToString(vm.GetMember(clip, "_name")).c_str(), n, n == 1 ? "y" : "ies");
      for (int e = 0; e < n && e < 10; ++e) {
        const swf::AsValue entry = vm.GetMember(entries, base::Format("{}", e));
        std::printf("    %s\n", vm.ToString(vm.GetMember(entry, "text")).c_str());
      }
      // What the list actually put on screen: its rows are timeline clips it
      // fills and shows, so an entry list with no visible row is a list the
      // player would see as empty.
      std::printf("    (top-half %s, text option %s, shown %s of %s)\n",
                  vm.ToString(vm.GetMember(clip, "iNumTopHalfEntries")).c_str(),
                  vm.ToString(vm.GetMember(clip, "textOption")).c_str(),
                  vm.ToString(vm.GetMember(clip, "iListItemsShown")).c_str(),
                  vm.ToString(vm.GetMember(clip, "iMaxItemsShown")).c_str());
      for (int r = 0; r < 16; ++r) {
        const swf::AsValue row = vm.GetMember(clip, base::Format("Entry{}", r));
        if (!row.is_object())
          break;
        std::printf("    row %d: y=%s h=%s visible=%d item=%s text='%s'\n", r,
                    vm.ToString(vm.GetMember(row, "_y")).c_str(),
                    vm.ToString(vm.GetMember(row, "_height")).c_str(),
                    vm.ToBool(vm.GetMember(row, "_visible")) ? 1 : 0,
                    vm.ToString(vm.GetMember(row, "itemIndex")).c_str(),
                    vm.ToString(vm.GetMember(vm.GetMember(row, "textField"), "text")).c_str());
      }
    }
    std::printf("%u list(s) came out with entries\n", filled);
  }

  // What there is to play: a menu keeps its states on labelled frames.
  u32 multiframe = 0;
  base::Vector<base::String> labels;
  for (const swf::Timeline& sprite : movie.sprites) {
    if (sprite.frames.size() > 1)
      ++multiframe;
    for (const swf::Frame& f : sprite.frames) {
      if (f.label.empty())
        continue;
      bool seen = false;
      for (const base::String& l : labels)
        seen = seen || l == f.label;
      if (!seen)
        labels.push_back(f.label);
    }
  }
  std::printf("%u multi-frame sprite(s), %zu distinct frame label(s)\n", multiframe,
              static_cast<size_t>(labels.size()));
  for (mem_size i = 0; i < labels.size() && i < 12; ++i)
    std::printf("  label %s\n", labels[i].c_str());

  // Run a few frames, so the timers a menu sets and its onEnterFrame work get
  // a chance to happen rather than sitting queued forever.
  u32 ticked = 0;
  for (int f = 0; f < 10; ++f)
    ticked += stage.Tick(1000.0 / 30.0);
  std::printf("%u timer(s) pending, %u handler(s) ran over 10 frames\n",
              vm.timer_count(), ticked);

  // Exercise the timeline on real assets: take a clip that carries states and
  // step it through each, reporting what the display list did.
  const base::Vector<swf::AsValue> stateful = stage.StatefulClips();
  std::printf("%zu clip(s) on the stage carry named frames\n",
              static_cast<size_t>(stateful.size()));
  for (mem_size i = 0; i < stateful.size() && i < 12; ++i) {
    base::String labels;
    for (const base::String& label : stage.LabelsOf(stateful[i])) {
      if (!labels.empty())
        labels += ", ";
      labels += label;
    }
    std::printf("  %-24s on frame %d of [%s]\n",
                vm.ToString(vm.GetMember(stateful[i], "_name")).c_str(),
                static_cast<int>(vm.ToNumber(vm.GetMember(stateful[i], "_currentframe"))),
                labels.c_str());
  }
  for (const swf::AsValue& clip : stateful) {
    const base::Vector<base::String> names = stage.LabelsOf(clip);
    if (names.size() < 2)
      continue;
    std::printf("  stepping '%s' through %zu state(s):\n",
                vm.ToString(vm.GetMember(clip, "_name")).c_str(),
                static_cast<size_t>(names.size()));
    for (const base::String& label : names) {
      if (!stage.GotoLabel(clip, label))
        continue;
      u32 children = 0;
      if (clip.is_object() && vm.Valid(clip.object()))
        for (const base::String& key : vm.Get(clip.object()).order)
          if (vm.GetMember(clip, key).is_object())
            ++children;
      std::printf("    %-18s frame %d, %u child object(s)\n", label.c_str(),
                  static_cast<int>(vm.ToNumber(vm.GetMember(clip, "_currentframe"))),
                  children);
    }
    break;
  }

  std::printf("%zu unclassed exported clip(s)\n",
              static_cast<size_t>(stage.unclassed().size()));
  std::printf("%u clip(s), %u with a registered class, %u frame change(s), "
              "%llu instruction(s)%s\n",
              stage.clip_count(), stage.classed_count(), stage.goto_count(),
              static_cast<unsigned long long>(vm.steps()),
              vm.exhausted() ? "  [step budget exhausted]" : "");

  const swf::AsObject& global = vm.Get(vm.global());
  u32 classes = 0;
  for (const base::String& name : global.order) {
    const swf::AsValue value = vm.GetMember(swf::AsValue::Obj(vm.global()), name);
    if (!value.is_object() || !vm.Valid(value.object()))
      continue;
    if (!vm.Get(value.object()).is_function)
      continue;
    ++classes;
    if (classes <= 24)
      std::printf("  _global.%s\n", name.c_str());
  }
  std::printf("%u function(s)/class(es) defined on _global\n", classes);
  // The tab strip, the clearest read on whether the authored component
  // parameters landed: each tab's label is set by a construct handler.
  for (u32 i = 1; i < vm.object_count(); ++i) {
    if (!vm.Valid(i) || !vm.Get(i).is_movie_clip)
      continue;
    const swf::AsValue clip = swf::AsValue::Obj(i);
    const base::String name = vm.ToString(vm.GetMember(clip, "_name"));
    if (name.find("Tab") == base::String::npos)
      continue;
    std::printf("  %s label '%s' text '%s'\n", name.c_str(),
                vm.ToString(vm.GetMember(clip, "label")).c_str(),
                vm.ToString(vm.GetMember(vm.GetMember(clip, "textField"), "text")).c_str());
  }
  const base::Vector<base::String> callbacks = bridge.Callbacks();
  if (!callbacks.empty()) {
    std::printf("%zu callback(s) the movie is listening for:\n",
                static_cast<size_t>(callbacks.size()));
    for (mem_size i = 0; i < callbacks.size() && i < 12; ++i)
      std::printf("  %s\n", callbacks[i].c_str());
  }

  // What the option list the start menu built came out as, which is the direct
  // check that sendMenuProperties reached the movie's own setup.
  for (u32 i = 1; i < 200000; ++i) {
    if (!vm.Valid(i) || !vm.Get(i).is_movie_clip)
      continue;
    const swf::AsValue clip = swf::AsValue::Obj(i);
    const swf::AsValue setup = vm.GetMember(clip, "setupMainMenu");
    if (!setup.is_object() || !vm.Valid(setup.object()) || !vm.Get(setup.object()).is_function)
      continue;
    const swf::AsValue list = vm.GetMember(clip, "MainList");
    // The property form, which is what the scripts actually write. It only
    // resolves once addProperty is honoured on the prototype chain.
    const swf::AsValue entries = vm.GetMember(list, "entryList");
    const int n = static_cast<int>(vm.ToNumber(vm.GetMember(entries, "length")));
    std::printf("setupMainMenu built %d entr%s:\n", n, n == 1 ? "y" : "ies");
    for (int e = 0; e < n; ++e) {
      const swf::AsValue entry = vm.GetMember(entries, base::Format("{}", e));
      std::printf("  %s\n", vm.ToString(vm.GetMember(entry, "text")).c_str());
    }
    break;
  }

  // What the screen asked its host for and nobody answered. This is the work
  // list for wiring a menu up: everything here is a blank the player can see.
  if (!bridge.pending().empty()) {
    std::printf("%zu unanswered call(s) to the host:\n",
                static_cast<size_t>(bridge.pending().size()));
    base::Vector<base::String> seen;
    for (const swf::GameBridge::Call& call : bridge.pending()) {
      bool known = false;
      for (const base::String& had : seen)
        known = known || had == call.name;
      if (known)
        continue;
      seen.push_back(call.name);
      if (seen.size() > 16)
        continue;
      base::String args;
      for (const swf::AsValue& arg : call.args) {
        if (!args.empty())
          args += ", ";
        args += vm.ToString(arg);
      }
      std::printf("  %s(%s)\n", call.name.c_str(), args.c_str());
    }
  }

  if (!vm.external_calls().empty()) {
    std::printf("%zu call(s) to the host bridge:\n",
                static_cast<size_t>(vm.external_calls().size()));
    base::Vector<base::String> seen;
    for (const base::String& name : vm.external_calls()) {
      bool known = false;
      for (const base::String& s : seen)
        known = known || s == name;
      if (known)
        continue;
      seen.push_back(name);
      if (seen.size() <= 20)
        std::printf("  %s\n", name.c_str());
    }
    std::printf("%zu distinct\n", static_cast<size_t>(seen.size()));
  }

  if (!vm.traces().empty()) {
    std::printf("%zu trace(s), first few:\n",
                static_cast<size_t>(vm.traces().size()));
    for (mem_size i = 0; i < vm.traces().size() && i < 8; ++i)
      std::printf("  %s\n", vm.traces()[i].c_str());
  }
}

int TranslateAll(const char* data_dir, const char* out_dir, f32 scale,
                 base::StringRef filter, bool reveal) {
  asset::Vfs vfs;
  MountData(vfs, data_dir);

  // The interface's own key table, so the screens read as the player sees them
  // rather than as "$LEVEL" and "$Saving...".
  // The language token differs by game: Skyrim spells it out, Fallout 4 and
  // Starfield use the two-letter code. Take whichever table is actually there.
  bethesda::InterfaceStrings strings;
  bool have_strings = false;
  for (const char* language : {"english", "en"}) {
    if (strings.Load(vfs, language)) {
      have_strings = true;
      break;
    }
  }
  if (have_strings) {
    std::printf("%zu interface string(s)\n", static_cast<size_t>(strings.size()));
    // Written out beside the screens: a host driving a menu the way the game
    // does needs the same table, and it has no other way to reach it.
    base::String table;
    for (const auto& entry : strings.entries()) {
      table += entry.key;
      table += '\t';
      table += entry.value;
      table += '\n';
    }
    WriteText(std::filesystem::path(out_dir) / "strings.txt", table);
  }

  // The typeface: the games embed it in the font movies rather than shipping a
  // font file, and interface/fontconfig.txt says which family each "$Font"
  // symbol means. Convert the faces, then resolve the symbols onto them.
  base::UnorderedMap<base::String, base::String> font_families;
  {
    const std::filesystem::path font_dir = std::filesystem::path(out_dir) / "fonts";
    bethesda::InterfaceFontConfig config;
    config.Load(vfs);

    // Only the libraries the config names, plus the shared gfxfontlib every
    // menu imports from. Pulling in the CJK sets as well would convert several
    // thousand glyphs the English interface never asks for.
    base::Vector<base::String> font_movies = config.libraries();
    vfs.Enumerate([&](base::StringRef path) {
      if (path.find("gfxfontlib") == base::StringRef::npos || !path.ends_with(".swf"))
        return;
      for (const base::String& known : font_movies)
        if (known == path)
          return;
      font_movies.push_back(base::String(path));
    });

    // family+style -> the converted file's family name, keeping whichever copy
    // carries the most glyphs when a face appears in several libraries.
    struct Face {
      base::String family;
      mem_size glyphs = 0;
    };
    base::UnorderedMap<base::String, Face> faces;
    auto key = [](base::StringRef family, bool bold, bool italic) {
      base::String out(family);
      out += bold ? "|b" : "|";
      out += italic ? "i" : "";
      return out;
    };

    u32 converted = 0;
    for (const base::String& path : font_movies) {
      auto bytes = vfs.Read(path);
      if (!bytes.has_value())
        continue;
      const base::Vector<u8>& data = bytes.value();
      auto font_file = swf::OpenSwf(ByteSpan{data.data(), data.size()});
      if (!font_file.has_value())
        continue;
      auto font_movie = swf::LoadMovie(font_file.value(), true);
      if (!font_movie.has_value())
        continue;
      for (const swf::Font& font : font_movie.value().fonts) {
        const base::String family = swf::FontFamilyName(font);
        const base::String id = key(font.name, font.bold, font.italic);
        Face* existing = faces.find(id);
        if (existing && existing->glyphs >= font.glyphs.size())
          continue;
        const base::Vector<u8> ttf = swf::ExportTrueType(font, family);
        if (ttf.empty())
          continue;
        const std::filesystem::path file =
            font_dir / (std::string(family.c_str()) + ".ttf");
        if (!WriteFile(file, ttf.data(), ttf.size()))
          continue;
        faces[id] = Face{family, font.glyphs.size()};
        // A movie may also name the font by its linkage symbol directly.
        const base::StringRef exported = font_movie.value().ExportName(font.id);
        if (!exported.empty())
          font_families[base::String(exported)] = family;
        if (!existing)
          ++converted;
      }
    }

    for (const auto& entry : config.maps()) {
      const Face* face =
          faces.find(key(entry.value.family, entry.value.bold, entry.value.italic));
      if (!face)
        face = faces.find(key(entry.value.family, false, false));
      if (face)
        font_families[entry.key] = face->family;
    }
    if (converted != 0)
      std::printf("%u font(s) converted into %s\n", converted,
                  font_dir.string().c_str());
  }

  base::Vector<base::String> movies;
  vfs.Enumerate([&](base::StringRef path) {
    if (!path.ends_with(".swf") && !path.ends_with(".gfx"))
      return;
    if (!filter.empty() && path.find(filter) == base::StringRef::npos)
      return;
    movies.push_back(base::String(path));
  });

  // Every movie stays loaded: a menu is spliced together from several of them
  // at runtime (an inventory screen pulls in its lists, item card and button
  // bar), and the translation follows the same imports.
  struct Loaded {
    base::String path;
    swf::SwfFile file;
    swf::Movie movie;
  };
  base::Vector<base::UniquePointer<Loaded>> loaded;
  base::Vector<swf::ImportedMovie> imports;
  for (const base::String& path : movies) {
    auto bytes = vfs.Read(path);
    if (!bytes.has_value())
      continue;
    const base::Vector<u8>& data = bytes.value();
    auto file = swf::OpenSwf(ByteSpan{data.data(), data.size()});
    if (!file.has_value())
      continue;
    auto movie = swf::LoadMovie(file.value());
    if (!movie.has_value())
      continue;
    auto entry = base::MakeUnique<Loaded>();
    entry->path = path;
    entry->file = base::move(file.value());
    entry->movie = base::move(movie.value());
    imports.push_back(swf::ImportedMovie{path, &entry->movie});
    loaded.push_back(base::move(entry));
  }

  u32 translated = 0;
  u32 skipped = 0;
  // A .gfx twin sits beside its .swf under exported/, so the file stem alone
  // collides; keep both rather than letting the second overwrite the first.
  base::Vector<base::String> used;
  for (mem_size m = 0; m < loaded.size(); ++m) {
    const base::String& path = loaded[m]->path;
    const swf::Movie& movie = loaded[m]->movie;

    swf::UguiExportOptions options;
    options.name = std::filesystem::path(path.c_str()).stem().string().c_str();
    for (u32 suffix = 2;; ++suffix) {
      bool taken = false;
      for (const base::String& name : used)
        taken = taken || name == options.name;
      if (!taken)
        break;
      options.name = base::Format("{}_{}",
                                  std::filesystem::path(path.c_str()).stem().string().c_str(),
                                  suffix);
    }
    used.push_back(options.name);
    options.scale = scale;
    options.reveal_faded = reveal;
    if (strings.size() != 0)
      options.strings = &strings.entries();
    if (font_families.size() != 0)
      options.font_families = &font_families;
    options.imports = &imports;

    swf::UguiScreen screen = swf::ExportUgui(movie, options);
    if (screen.widget_count <= 1) {
      // A movie that is only a script stub has nothing to lay out.
      ++skipped;
      continue;
    }

    const std::filesystem::path dir = out_dir;
    const std::string stem = options.name.c_str();
    WriteText(dir / (stem + ".ugui"), screen.markup);
    WriteText(dir / (stem + ".as"), screen.script);
    WriteText(dir / (stem + ".manifest"), screen.manifest);
    for (const swf::ExportedAsset& asset : screen.assets)
      WriteFile(dir / asset.file.c_str(), asset.bytes.data(), asset.bytes.size());
    // The movie itself goes beside its translation, so a host can run the
    // screen's own code without reaching back into the game's archives.
    if (auto bytes = vfs.Read(path)) {
      const base::Vector<u8>& raw = bytes.value();
      WriteFile(dir / (stem + ".swf"), raw.data(), raw.size());
    }
    std::printf("  %-34s %u widgets, %zu assets\n", stem.c_str(), screen.widget_count,
                static_cast<size_t>(screen.assets.size()));
    ++translated;
  }
  std::printf("%u movie(s) translated into %s, %u skipped\n", translated, out_dir,
              skipped);
  return translated == 0 ? 1 : 0;
}

int Usage() {
  std::printf(
      "usage: swfdump <file.swf|file.gfx> [mode]\n"
      "  (default)          summary of the movie\n"
      "  --tags             every tag in order\n"
      "  --exports          exported linkage symbols\n"
      "  --text             edit-text fields and their ActionScript bindings\n"
      "  --script           decompiled ActionScript 2 for the whole movie\n"
      "  --disasm           AVM1 disassembly listing\n"
      "  --strings          every constant-pool string\n"
      "  --fonts <out-dir>  convert the movie's embedded fonts to TrueType\n"
      "  --ugui <out-dir> [frame] [scale]\n"
      "                     translate to libultragui markup plus its assets;\n"
      "                     scale 1.5 fits a 720p Bethesda stage to a 1080p ui\n"
      "\n"
      "       swfdump --data <data-dir> --ugui-all <out-dir> [scale] [filter]\n"
      "                     translate every movie in a game's archives at once\n"
      "\n"
      "  --reveal           show what the movie is authored to look like: a\n"
      "                     Scaleform menu ships transparent and fades itself in\n");
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2)
    return Usage();

  // A flag rather than another positional: it applies to both translate modes.
  bool reveal = false;
  base::Vector<char*> args;
  for (int i = 0; i < argc; ++i) {
    if (base::StringRef(argv[i]) == "--reveal")
      reveal = true;
    else
      args.push_back(argv[i]);
  }
  argc = static_cast<int>(args.size());
  argv = args.data();
  if (argc < 2)
    return Usage();  // the count above was checked before --reveal was removed

  if (base::StringRef(argv[1]) == "--data") {
    if (argc < 5 || base::StringRef(argv[3]) != "--ugui-all")
      return Usage();
    const f32 scale = argc > 5 ? static_cast<f32>(std::atof(argv[5])) : 1.0f;
    const base::StringRef filter = argc > 6 ? base::StringRef(argv[6]) : base::StringRef();
    return TranslateAll(argv[2], argv[4], scale, filter, reveal);
  }

  const base::Vector<u8> bytes = ReadFile(argv[1]);
  if (bytes.empty()) {
    std::printf("cannot read %s\n", argv[1]);
    return 1;
  }

  auto file = swf::OpenSwf(ByteSpan{bytes.data(), bytes.size()});
  if (!file.has_value()) {
    std::printf("%s is not a readable swf/gfx movie\n", argv[1]);
    return 1;
  }

  auto movie = swf::LoadMovie(file.value());
  if (!movie.has_value()) {
    std::printf("%s decoded no characters\n", argv[1]);
    return 1;
  }

  const base::StringRef mode = argc > 2 ? base::StringRef(argv[2]) : base::StringRef();
  if (mode.empty()) {
    PrintSummary(file.value(), movie.value());
    return 0;
  }
  if (mode == "--tags") {
    PrintTags(file.value());
    return 0;
  }
  if (mode == "--exports") {
    PrintExports(movie.value());
    return 0;
  }
  if (mode == "--text") {
    PrintTexts(movie.value());
    return 0;
  }
  if (mode == "--script") {
    const base::String script = swf::ExportScript(movie.value());
    std::fwrite(script.data(), 1, script.size(), stdout);
    return 0;
  }
  if (mode == "--run") {
    RunScripts(movie.value());
    return 0;
  }
  if (mode == "--disasm") {
    for (const swf::Script& script : movie.value().scripts) {
      std::printf("// %s %u, sprite %u\n",
                  script.kind == swf::Script::Kind::kInit ? "initclip" : "frame",
                  script.frame, script.sprite_id);
      const base::String listing = swf::Disassembly(script.code);
      std::fwrite(listing.data(), 1, listing.size(), stdout);
    }
    return 0;
  }
  if (mode == "--strings") {
    for (const swf::Script& script : movie.value().scripts) {
      for (const base::String& s : swf::ConstantStrings(script.code))
        std::printf("%s\n", s.c_str());
    }
    return 0;
  }
  if (mode == "--fonts") {
    if (argc < 4)
      return Usage();
    auto with_glyphs = swf::LoadMovie(file.value(), true);
    if (!with_glyphs.has_value())
      return 1;
    const std::filesystem::path out_dir = argv[3];
    int written = 0;
    for (const swf::Font& font : with_glyphs.value().fonts) {
      const base::String family = swf::FontFamilyName(font);
      const base::Vector<u8> ttf = swf::ExportTrueType(font, family);
      if (ttf.empty()) {
        std::printf("  %-28s no glyph outlines\n", family.c_str());
        continue;
      }
      const std::filesystem::path path = out_dir / (std::string(family.c_str()) + ".ttf");
      if (!WriteFile(path, ttf.data(), ttf.size()))
        continue;
      std::printf("  %-28s %zu glyphs, %zu bytes\n", family.c_str(),
                  static_cast<size_t>(font.glyphs.size()),
                  static_cast<size_t>(ttf.size()));
      ++written;
    }
    std::printf("%d font(s) written to %s\n", written, out_dir.string().c_str());
    return written == 0 ? 1 : 0;
  }
  if (mode == "--ugui") {
    if (argc < 4)
      return Usage();
    const std::filesystem::path out_dir = argv[3];
    swf::UguiExportOptions options;
    options.name = std::filesystem::path(argv[1]).stem().string().c_str();
    if (argc > 4)
      options.frame = static_cast<u32>(std::atoi(argv[4]));
    if (argc > 5)
      options.scale = static_cast<f32>(std::atof(argv[5]));
    options.reveal_faded = reveal;

    swf::UguiScreen screen = swf::ExportUgui(movie.value(), options);
    const std::filesystem::path markup = out_dir / (options.name.c_str() + std::string(".ugui"));
    if (!WriteText(markup, screen.markup)) {
      std::printf("cannot write %s\n", markup.string().c_str());
      return 1;
    }
    WriteText(out_dir / (options.name.c_str() + std::string(".as")), screen.script);
    WriteText(out_dir / (options.name.c_str() + std::string(".manifest")),
              screen.manifest);
    for (const swf::ExportedAsset& asset : screen.assets)
      WriteFile(out_dir / asset.file.c_str(), asset.bytes.data(), asset.bytes.size());

    std::printf("%s: %u widgets, %zu assets, %u display objects skipped\n",
                markup.string().c_str(), screen.widget_count,
                static_cast<size_t>(screen.assets.size()), screen.skipped_count);
    return 0;
  }

  return Usage();
}
