// pexdump: parse a Skyrim/Fallout .pex out of the game archives and print it,
// validating the Papyrus container parser against shipped scripts.
//
//   pexdump <data_dir> <ScriptName> [--disasm]
//
// ScriptName is the script's object name without extension, e.g. "Actor" reads
// scripts/actor.pex from whichever mounted archive provides it.

#include <cstdio>
#include <filesystem>
#include <string>

#include "asset/vfs.h"
#include "bethesda/archive.h"
#include "script/papyrus/opcode.h"
#include "script/papyrus/pex.h"

namespace {

using namespace rec;
using namespace rec::script::papyrus;

void MountArchives(asset::Vfs& vfs, const std::string& data_dir) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec)) {
    std::string path = entry.path().string();
    if (auto provider = bethesda::OpenArchive(path)) vfs.Mount(std::move(provider));
  }
}

std::string FormatOperand(const PexFile& pex, const VariableData& v) {
  switch (v.type) {
    case VariableData::Type::kNone:
      return "None";
    case VariableData::Type::kIdentifier:
      return pex.Str(v.string_index);
    case VariableData::Type::kString:
      return "\"" + pex.Str(v.string_index) + "\"";
    case VariableData::Type::kInteger:
      return std::to_string(v.int_value);
    case VariableData::Type::kFloat:
      return std::to_string(v.float_value);
    case VariableData::Type::kBool:
      return v.bool_value ? "true" : "false";
  }
  return "?";
}

void PrintFunction(const PexFile& pex, const std::string& name, const Function& fn, bool disasm) {
  std::printf("    %s %s(", pex.Str(fn.return_type).c_str(), name.c_str());
  for (size_t i = 0; i < fn.params.size(); ++i) {
    std::printf("%s%s %s", i ? ", " : "", pex.Str(fn.params[i].type).c_str(),
                pex.Str(fn.params[i].name).c_str());
  }
  std::printf(")%s%s, %zu locals, %zu instrs\n", fn.is_global ? " global" : "",
              fn.is_native ? " native" : "", fn.locals.size(), fn.code.size());
  if (!disasm) return;
  for (size_t i = 0; i < fn.code.size(); ++i) {
    const Instruction& insn = fn.code[i];
    std::printf("      %4zu  %-20s", i, GetOpInfo(insn.op).mnemonic);
    for (const VariableData& a : insn.args) std::printf(" %s", FormatOperand(pex, a).c_str());
    if (!insn.var_args.empty()) {
      std::printf(" [");
      for (size_t j = 0; j < insn.var_args.size(); ++j)
        std::printf("%s%s", j ? ", " : "", FormatOperand(pex, insn.var_args[j]).c_str());
      std::printf("]");
    }
    std::printf("\n");
  }
}

int Dump(const std::string& data_dir, const std::string& script, bool disasm) {
  asset::Vfs vfs;
  MountArchives(vfs, data_dir);
  std::printf("mounted %zu archives\n", vfs.mount_count());

  std::string path = "scripts/" + script + ".pex";
  auto blob = vfs.Read(path);
  if (!blob) {
    std::printf("not found: %s\n", path.c_str());
    return 1;
  }
  std::printf("read %s (%zu bytes)\n", path.c_str(), static_cast<size_t>(blob->size()));

  PexFile pex;
  if (!ParsePex(ByteSpan(blob->data(), blob->size()), &pex)) {
    std::printf("parse failed\n");
    return 1;
  }

  std::printf("source=%s game_id=%u version=%u.%u %s-endian\n", pex.source_file_name.c_str(),
              pex.game_id, pex.major_version, pex.minor_version, pex.big_endian ? "big" : "little");
  std::printf("strings=%zu user_flags=%zu objects=%zu debug=%s\n", pex.string_table.size(),
              pex.user_flags.size(), pex.objects.size(), pex.has_debug_info ? "yes" : "no");

  for (const Object& obj : pex.objects) {
    std::printf("object %s", pex.Str(obj.name).c_str());
    if (obj.parent_class != kInvalidString && !pex.Str(obj.parent_class).empty())
      std::printf(" extends %s", pex.Str(obj.parent_class).c_str());
    std::printf(" (%zu vars, %zu props, %zu states)\n", obj.variables.size(),
                obj.properties.size(), obj.states.size());
    for (const Property& p : obj.properties) {
      std::printf("  property %s %s%s\n", pex.Str(p.type).c_str(), pex.Str(p.name).c_str(),
                  p.is_auto() ? " auto" : "");
      if (p.has_getter) PrintFunction(pex, "get", p.getter, disasm);
      if (p.has_setter) PrintFunction(pex, "set", p.setter, disasm);
    }
    for (const State& s : obj.states) {
      std::string sname = pex.Str(s.name);
      std::printf("  state %s (%zu functions)\n", sname.empty() ? "<default>" : sname.c_str(),
                  s.functions.size());
      for (const NamedFunction& nf : s.functions)
        PrintFunction(pex, pex.Str(nf.name), nf.function, disasm);
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <data_dir> <ScriptName> [--disasm]\n", argv[0]);
    return 2;
  }
  bool disasm = argc > 3 && std::string(argv[3]) == "--disasm";
  return Dump(argv[1], argv[2], disasm);
}
