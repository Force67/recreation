#include <base/algorithm.h>
#include <base/containers/unordered_map.h>
#include <base/memory/move.h>
#include <base/strings/string_ref.h>
#include <base/strings/xstring.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>

#include "asset/vfs.h"
#include "components/bethesda/archive.h"
#include "components/bethesda/shader_package.h"
#include "core/log.h"

// Dumps a game's shipped shader library. Every Bethesda title keeps its whole
// compiled shader set in one blob (shadersfx/*.fxp inside the Shaders archive,
// or Data/Shaders/*.sdp on the older engines); this walks the container and
// reports, or writes out, what is in there.
//
//   shaderdump <data-dir>                     summary of every package found
//   shaderdump <data-dir> --group 6           per-shader listing for one group
//   shaderdump <data-dir> --out <dir>         write bytecode + manifest.tsv
//
// The manifest is the point: .fxp blobs have their reflection chunk stripped,
// so the technique id, vertex layout and constant table travel beside the
// bytecode rather than in it.

namespace {

using namespace rx;

struct Options {
  base::String data_dir;
  base::String out_dir;
  // rx::i64 and base/arch.h's i64 are different types sharing a global name,
  // so the 64-bit spelling is qualified; see assetdump.
  rx::i64 group = -1;
  u32 limit = 0;
  bool techniques = false;
  bool map = false;
};

bool IsShaderPackage(base::StringRef path) {
  return path.ends_with(".fxp") || path.ends_with(".sdp");
}

std::optional<base::Vector<u8>> ReadLooseFile(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec)
    return std::nullopt;
  std::FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f)
    return std::nullopt;
  base::Vector<u8> data(static_cast<size_t>(size));
  const size_t read = std::fread(data.data(), 1, data.size(), f);
  std::fclose(f);
  if (read != data.size())
    return std::nullopt;
  return data;
}

// Last path component, for naming the per-package output directory.
base::String BaseName(const base::String& path) {
  size_t start = 0;
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '/' || path[i] == '\\')
      start = i + 1;
  }
  return base::String(path.c_str() + start);
}

// A shader's name is 256 bytes the package chose, and --out turns it into a
// path. Left alone, a name of "../../.bashrc" walks out of the output directory
// and an absolute one replaces it outright, because `dir / name` on an absolute
// path discards `dir`. Since a .sdp can come from a mod, that is a file write
// wherever the user can write.
//
// So the name is not used as a path at all: everything outside a small safe
// alphabet becomes '_', which leaves no separators, no drive letter and no way
// to spell a parent directory. The result is a label on a filename this tool
// builds, never a path from the file.
base::String SafeFileName(const base::String& raw) {
  base::String out;
  for (char c : raw) {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    // A run of unsafe bytes collapses to one '_' so "../../x" cannot become a
    // name that is mostly underscores, and a leading dot cannot start it (which
    // would hide the file, and lets ".." through as "..").
    if (safe && !(c == '.' && out.empty()))
      out.push_back(c);
    else if (!out.empty() && out[out.size() - 1] != '_')
      out.push_back('_');
  }
  // Trailing dots and spaces are not legal filename endings on Windows.
  while (!out.empty() && (out[out.size() - 1] == '.' || out[out.size() - 1] == '_'))
    out.pop_back();
  if (out.size() > 96)
    out = base::String(out.c_str(), 96);
  return out.empty() ? base::String("shader") : out;
}

void PrintSummary(const base::String& path, const bethesda::ShaderPackage& package) {
  u32 by_stage[7] = {};
  for (const bethesda::PackagedShader& s : package.shaders)
    ++by_stage[static_cast<u32>(s.stage)];

  std::printf("%s: %zu shaders in %zu groups%s\n", path.c_str(),
              static_cast<size_t>(package.shaders.size()),
              static_cast<size_t>(package.groups.size()), package.valid ? "" : "  (INCOMPLETE)");
  std::printf("  stages:");
  for (u32 i = 0; i < 7; ++i) {
    if (by_stage[i] != 0)
      std::printf(" %s=%u", bethesda::ShaderStageName(static_cast<bethesda::ShaderStage>(i)),
                  by_stage[i]);
  }
  std::printf("\n");

  for (size_t g = 0; g < package.groups.size(); ++g) {
    const bethesda::ShaderGroup& group = package.groups[g];
    // The long tail of the shipped packages is one-permutation groups; only the
    // interesting ones are worth a line each.
    if (group.shader_count <= 2 && package.groups.size() > 32)
      continue;
    std::printf("  group %-4zu vs=%-5u ps=%-5u shaders=%u\n", g, group.vertex_count,
                group.pixel_count, group.shader_count);
  }
}

void PrintGroup(const bethesda::ShaderPackage& package, u32 group, u32 limit) {
  u32 shown = 0;
  for (const bethesda::PackagedShader& s : package.shaders) {
    if (s.group != group)
      continue;
    if (limit != 0 && shown >= limit) {
      std::printf("  ...\n");
      break;
    }
    std::printf("  %-3s tech=0x%08x bytes=%-6zu", bethesda::ShaderStageName(s.stage),
                s.technique_id, static_cast<size_t>(s.bytecode.size()));
    if (s.vertex_desc != 0)
      std::printf(" vertexdesc=%#018llx", static_cast<unsigned long long>(s.vertex_desc));
    if (!s.name.empty())
      std::printf(" %s", s.name.c_str());
    std::printf("\n");
    ++shown;
  }
}

// The cost of flipping one technique bit on, summed over matched pairs.
struct BitEffect {
  u32 pairs = 0;
  i32 textures = 0;
  i32 samplers = 0;
  i32 vectors = 0;
  i32 instructions = 0;
};

// The technique id is a bitfield of material features, and the package never
// says which bit is which. What it does say is what each permutation compiled
// down to, so the bit's meaning falls out of comparing shaders whose ids differ
// in that bit ALONE: the bit that adds a texture and a sampler switches a map
// on, the bit that only adds instructions is a lighting or blending mode.
// Averaging over all shaders instead would blur the bits together, since the
// permutations are far from evenly spread.
void PrintTechniqueBits(const bethesda::ShaderPackage& package,
                        u32 group,
                        bethesda::ShaderStage stage) {
  // A technique id repeats within a stage: the same technique is compiled once
  // per vertex layout. Keyed by technique alone, only the first survives and the
  // bit cost is then a difference between two shaders that draw different
  // geometry. Keep every variant, and only ever compare two that share a layout.
  struct Variant {
    rx::u64 vertex_desc = 0;
    bethesda::ShaderReflection reflection;
  };
  base::UnorderedMap<u32, base::Vector<Variant>> by_technique;
  for (const bethesda::PackagedShader& s : package.shaders) {
    if (s.group != group || s.stage != stage)
      continue;
    const bethesda::ShaderReflection r = bethesda::ReflectShader(s.bytecode);
    if (r.valid)
      by_technique[s.technique_id].push_back({s.vertex_desc, r});
  }
  if (by_technique.empty())
    return;

  BitEffect effects[32];
  for (const auto& [technique, off_variants] : by_technique) {
    for (u32 bit = 0; bit < 32; ++bit) {
      if ((technique >> bit) & 1)
        continue;
      const base::Vector<Variant>* on_variants = by_technique.find(technique | (1u << bit));
      if (!on_variants)
        continue;
      for (const Variant& off : off_variants) {
        const bethesda::ShaderReflection* on = nullptr;
        for (const Variant& candidate : *on_variants) {
          if (candidate.vertex_desc == off.vertex_desc) {
            on = &candidate.reflection;
            break;
          }
        }
        if (!on)
          continue;
        BitEffect& e = effects[bit];
        ++e.pairs;
        e.textures += static_cast<i32>(on->textures) - static_cast<i32>(off.reflection.textures);
        e.samplers += static_cast<i32>(on->samplers) - static_cast<i32>(off.reflection.samplers);
        e.vectors += static_cast<i32>(on->constant_buffer_vectors) -
                     static_cast<i32>(off.reflection.constant_buffer_vectors);
        e.instructions += static_cast<i32>(on->instructions) -
                          static_cast<i32>(off.reflection.instructions);
      }
    }
  }

  std::printf("group %u %s: %zu techniques, cost of setting each bit (mean over matched pairs)\n",
              group, bethesda::ShaderStageName(stage),
              static_cast<size_t>(by_technique.size()));
  std::printf("  bit   pairs   textures  samplers  cb_float4s  instructions\n");
  for (u32 bit = 0; bit < 32; ++bit) {
    const BitEffect& e = effects[bit];
    if (e.pairs == 0)
      continue;
    const double n = e.pairs;
    std::printf("  %-3u  %6u   %+8.2f  %+8.2f  %+10.1f  %+12.1f\n", bit, e.pairs,
                e.textures / n, e.samplers / n, e.vectors / n, e.instructions / n);
  }
}

// What a group's shaders have in common, which is the evidence for what the
// unnamed group is. Vertex inputs say what geometry the class draws, render
// target counts say which pass it belongs to, and an oversized constant buffer
// is a bone palette.
struct GroupEvidence {
  base::Vector<base::String> inputs;  // distinct vertex semantics
  u32 max_textures = 0;
  u32 max_samplers = 0;
  u32 max_targets = 0;
  u32 max_cb_vectors = 0;
};

void NoteInput(GroupEvidence* e, const base::String& semantic) {
  for (const base::String& seen : e->inputs) {
    if (seen == semantic)
      return;
  }
  e->inputs.push_back(semantic);
}

void PrintMap(const bethesda::ShaderPackage& package) {
  for (size_t g = 0; g < package.groups.size(); ++g) {
    const bethesda::ShaderGroup& group = package.groups[g];
    // The one-permutation tail is post-effect blits; they say little and there
    // are hundreds of them.
    if (group.shader_count <= 2 && package.groups.size() > 32)
      continue;

    GroupEvidence evidence;
    for (u32 i = 0; i < group.shader_count; ++i) {
      const bethesda::PackagedShader& s = package.shaders[group.first_shader + i];
      const bethesda::ShaderReflection r = bethesda::ReflectShader(s.bytecode);
      if (r.valid) {
        evidence.max_textures = base::Max(evidence.max_textures, r.textures);
        evidence.max_samplers = base::Max(evidence.max_samplers, r.samplers);
        evidence.max_cb_vectors = base::Max(evidence.max_cb_vectors, r.constant_buffer_vectors);
      }
      const bethesda::ShaderSignatures sig = bethesda::ReflectSignatures(s.bytecode);
      if (!sig.valid)
        continue;
      if (s.stage == bethesda::ShaderStage::kVertex) {
        for (const base::String& in : sig.inputs)
          NoteInput(&evidence, in);
      } else if (s.stage == bethesda::ShaderStage::kPixel) {
        u32 targets = 0;
        for (const base::String& out : sig.outputs) {
          if (out.find("SV_Target") != base::String::npos ||
              out.find("SV_TARGET") != base::String::npos)
            ++targets;
        }
        evidence.max_targets = base::Max(evidence.max_targets, targets);
      }
    }

    std::printf("group %-3zu vs=%-5u ps=%-5u cs=%-3u | tex<=%-2u samp<=%-2u rt<=%u cbvec<=%u\n", g,
                group.vertex_count, group.pixel_count, group.compute_count, evidence.max_textures,
                evidence.max_samplers, evidence.max_targets, evidence.max_cb_vectors);
    std::printf("          inputs:");
    for (const base::String& in : evidence.inputs)
      std::printf(" %s", in.c_str());
    std::printf("\n");
  }
}

// .sdp packages kept their names, so the catalogue of what a shader library
// actually contains is readable straight off them. Grouping by the leading
// letters is enough to see the families.
void PrintNameCatalogue(const bethesda::ShaderPackage& package) {
  base::UnorderedMap<base::String, u32> families;
  for (const bethesda::PackagedShader& s : package.shaders) {
    base::String prefix;
    for (size_t i = 0; i < s.name.size(); ++i) {
      const char c = s.name[i];
      if (c == '.' || (c >= '0' && c <= '9'))
        break;
      prefix.push_back(c);
    }
    if (prefix.empty())
      prefix = "?";
    ++families[prefix];
  }
  std::printf("  families:");
  for (const auto& [prefix, count] : families)
    std::printf(" %s=%u", prefix.c_str(), count);
  std::printf("\n");
}

// One shader per file, plus a manifest row carrying what the bytecode lost.
bool WritePackage(const base::String& out_dir,
                  const base::String& package_name,
                  const bethesda::ShaderPackage& package) {
  std::error_code ec;
  std::filesystem::path dir = std::filesystem::path(out_dir.c_str()) / package_name.c_str();
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    std::printf("cannot create %s: %s\n", dir.string().c_str(), ec.message().c_str());
    return false;
  }

  std::FILE* manifest = std::fopen((dir / "manifest.tsv").string().c_str(), "wb");
  if (!manifest) {
    std::printf("cannot write manifest in %s\n", dir.string().c_str());
    return false;
  }
  std::fprintf(manifest, "file\tgroup\tstage\ttechnique\tbytes\tvertex_desc\tdescriptor\n");

  char name[320];
  u32 written = 0, failed = 0;
  for (size_t i = 0; i < package.shaders.size(); ++i) {
    const bethesda::PackagedShader& s = package.shaders[i];
    const char* stage = bethesda::ShaderStageName(s.stage);
    // .sdp shaders are named; .fxp ones are identified by group + technique.
    // Either way the index goes in the filename: a group repeats a technique id
    // per stage, and a shipped .sdp repeats a shader name, so without it later
    // records silently overwrite earlier ones while the manifest claims both.
    if (!s.name.empty()) {
      const base::String safe = SafeFileName(s.name);
      std::snprintf(name, sizeof(name), "%zu_%s.dxbc", i, safe.c_str());
    } else {
      std::snprintf(name, sizeof(name), "g%03u_%s_%08x_%zu.dxbc", s.group, stage, s.technique_id,
                    i);
    }

    std::FILE* f = std::fopen((dir / name).string().c_str(), "wb");
    if (!f) {
      ++failed;
      continue;
    }
    if (s.bytecode.size() != 0)
      std::fwrite(s.bytecode.data(), 1, s.bytecode.size(), f);
    std::fclose(f);
    ++written;

    std::fprintf(manifest, "%s\t%u\t%s\t0x%08x\t%zu\t0x%016llx\t", name, s.group, stage,
                 s.technique_id, static_cast<size_t>(s.bytecode.size()),
                 static_cast<unsigned long long>(s.vertex_desc));
    for (size_t b = 0; b < s.descriptor.size(); ++b)
      std::fprintf(manifest, "%02x", s.descriptor.data()[b]);
    std::fprintf(manifest, "\n");
  }
  std::fclose(manifest);
  if (failed != 0)
    std::printf("  %u shaders could not be opened for writing in %s\n", failed,
                dir.string().c_str());
  std::printf("  wrote %u shaders to %s\n", written, dir.string().c_str());
  return failed == 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf(
        "usage: shaderdump <data-dir> [--out <dir>] [--group <n>] [--limit <n>]\n"
        "  --out         write every shader's bytecode plus manifest.tsv\n"
        "  --group       list the shaders of one group instead of the summary\n"
        "  --techniques  with --group, show what each technique bit costs\n"
        "  --map         per-group evidence for what each unnamed group draws\n");
    return 1;
  }

  Options options;
  options.data_dir = argv[1];
  for (int i = 2; i < argc; ++i) {
    base::StringRef arg = argv[i];
    if (arg == "--out" && i + 1 < argc)
      options.out_dir = argv[++i];
    else if (arg == "--group" && i + 1 < argc)
      options.group = std::atoll(argv[++i]);
    else if (arg == "--limit" && i + 1 < argc)
      options.limit = static_cast<u32>(std::atoll(argv[++i]));
    else if (arg == "--techniques")
      options.techniques = true;
    else if (arg == "--map")
      options.map = true;
    else {
      std::printf("unknown argument: %s\n", argv[i]);
      return 1;
    }
  }

  if (std::getenv("RX_LOG_DEBUG"))
    rx::SetLogLevel(rx::LogLevel::kDebug);

  // Mount the archives and the loose tree, so packages are found whether they
  // ship inside a BSA/BA2 (Skyrim SE, Fallout 4, Starfield) or as loose files
  // under Data/Shaders (Oblivion, Fallout 3, New Vegas).
  asset::Vfs vfs;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(options.data_dir.c_str(), ec)) {
    if (auto provider = bethesda::OpenArchive(entry.path().string()))
      vfs.Mount(base::move(provider));
  }
  if (ec) {
    std::printf("cannot read data dir %s: %s\n", options.data_dir.c_str(), ec.message().c_str());
    return 1;
  }
  base::Vector<base::String> archived;
  vfs.Enumerate([&](base::StringRef path) {
    if (IsShaderPackage(path))
      archived.push_back(base::String(path));
  });

  // Loose packages are read straight off disk rather than through a loose-file
  // mount: the vfs lower-cases paths, which does not resolve against the
  // shipped "Data/Shaders" on a case-sensitive filesystem.
  base::Vector<base::String> loose;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(options.data_dir.c_str(), ec)) {
    base::String path = entry.path().string();
    base::String lowered = path;
    for (char& c : lowered)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (entry.is_regular_file() && IsShaderPackage(lowered))
      loose.push_back(base::move(path));
  }

  if (archived.empty() && loose.empty()) {
    std::printf("no shader packages under %s\n", options.data_dir.c_str());
    return 1;
  }

  bool write_failed = false;
  for (size_t i = 0; i < archived.size() + loose.size(); ++i) {
    const bool from_archive = i < archived.size();
    const base::String& path = from_archive ? archived[i] : loose[i - archived.size()];
    auto bytes = from_archive ? vfs.Read(path) : ReadLooseFile(path.c_str());
    if (!bytes) {
      std::printf("%s: unreadable\n", path.c_str());
      continue;
    }
    bethesda::ShaderPackage package =
        bethesda::ParseShaderPackage(rx::ByteSpan(bytes->data(), bytes->size()));
    if (package.shaders.empty()) {
      std::printf("%s: not a shader package the parser knows (%zu bytes)\n", path.c_str(),
                  static_cast<size_t>(bytes->size()));
      continue;
    }

    if (options.group >= 0) {
      const u32 group = static_cast<u32>(options.group);
      if (options.techniques) {
        // Every stage the package can hold, not just the two Skyrim's lighting
        // groups use: Fallout 4 puts hull and domain shaders in its groups, and
        // both games have compute groups. Hard-coding vs/ps made --techniques
        // print nothing at all for those, which reads as "no technique bits".
        for (bethesda::ShaderStage stage :
             {bethesda::ShaderStage::kVertex, bethesda::ShaderStage::kPixel,
              bethesda::ShaderStage::kGeometry, bethesda::ShaderStage::kHull,
              bethesda::ShaderStage::kDomain, bethesda::ShaderStage::kCompute})
          PrintTechniqueBits(package, group, stage);
      } else {
        std::printf("%s group %u:\n", path.c_str(), group);
        PrintGroup(package, group, options.limit);
      }
      continue;
    }

    if (options.map) {
      std::printf("%s\n", path.c_str());
      if (package.groups.empty())
        PrintNameCatalogue(package);
      else
        PrintMap(package);
      continue;
    }

    PrintSummary(path, package);
    // A dump that wrote nothing has to say so in its exit status, or a script
    // driving this reports success over an empty directory.
    if (!options.out_dir.empty() && !WritePackage(options.out_dir, BaseName(path), package))
      write_failed = true;
  }
  return write_failed ? 1 : 0;
}
