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

  char name[256];
  u32 written = 0;
  for (size_t i = 0; i < package.shaders.size(); ++i) {
    const bethesda::PackagedShader& s = package.shaders[i];
    const char* stage = bethesda::ShaderStageName(s.stage);
    // .sdp shaders are named; .fxp ones are identified by group + technique,
    // with the index appended because a group can repeat a technique id per
    // stage.
    if (!s.name.empty())
      std::snprintf(name, sizeof(name), "%s", s.name.c_str());
    else
      std::snprintf(name, sizeof(name), "g%03u_%s_%08x_%zu.dxbc", s.group, stage, s.technique_id,
                    i);

    std::FILE* f = std::fopen((dir / name).string().c_str(), "wb");
    if (!f)
      continue;
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
  std::printf("  wrote %u shaders to %s\n", written, dir.string().c_str());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf(
        "usage: shaderdump <data-dir> [--out <dir>] [--group <n>] [--limit <n>]\n"
        "  --out    write every shader's bytecode plus manifest.tsv\n"
        "  --group  list the shaders of one group instead of the summary\n");
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
      std::printf("%s group %lld:\n", path.c_str(), static_cast<long long>(options.group));
      PrintGroup(package, static_cast<u32>(options.group), options.limit);
      continue;
    }

    PrintSummary(path, package);
    if (!options.out_dir.empty())
      WritePackage(options.out_dir, BaseName(path), package);
  }
  return 0;
}
