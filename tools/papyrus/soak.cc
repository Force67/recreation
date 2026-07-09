// pexsoak: load every shipped Papyrus script through the VM with the full
// native table bound, instantiate a sample, and (with --exec) execute the
// straight-line bytecode functions, stressing the parser, the inheritance/
// loader, native dispatch, and the interpreter's opcode handling against the
// entire real corpus. A clean run over all ~14k scripts is the broad
// robustness check the unit tests can't give.
//
//   pexsoak <data_dir> [instantiate_stride] [--exec]
//
// --exec  also run every non-native, no-parameter function whose body has no
//         calls and no backward jumps (so it is guaranteed to terminate without
//         recursion), exercising the interpreter on real compiled bytecode.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "asset/vfs.h"
#include "bethesda/archive.h"
#include "script/games/skyrim/skyrim_natives.h"
#include "script/papyrus/native.h"
#include "script/papyrus/opcode.h"
#include "script/papyrus/pex.h"
#include "script/papyrus/vm.h"

using namespace rx;
using namespace rx::script::papyrus;

// A function is safe to execute blind when it has no calls (no recursion, no
// latent natives) and no backward jumps (no loops), so it always terminates.
bool IsStraightLine(const Function& fn) {
  if (fn.is_native || !fn.params.empty()) return false;
  for (const Instruction& in : fn.code) {
    switch (in.op) {
      case Op::kCallMethod:
      case Op::kCallParent:
      case Op::kCallStatic:
        return false;
      case Op::kJmp:
        if (in.args.empty() || in.args[0].type != VariableData::Type::kInteger ||
            in.args[0].int_value < 0)
          return false;
        break;
      case Op::kJmpT:
      case Op::kJmpF:
        if (in.args.size() < 2 || in.args[1].type != VariableData::Type::kInteger ||
            in.args[1].int_value < 0)
          return false;
        break;
      default:
        break;
    }
  }
  return true;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <data_dir> [instantiate_stride] [--exec]\n", argv[0]);
    return 2;
  }
  int stride = 16;
  bool exec = false;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--exec") == 0) exec = true;
    else if (std::atoi(argv[i]) > 0) stride = std::atoi(argv[i]);
  }

  std::vector<base::UniquePointer<asset::FileProvider>> providers;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(argv[1], ec))
    if (auto p = bethesda::OpenArchive(e.path().string())) providers.push_back(std::move(p));

  std::map<std::string, size_t> scripts;
  for (size_t i = 0; i < providers.size(); ++i)
    providers[i]->Enumerate([&](std::string_view path) {
      if (path.size() > 12 && path.substr(0, 8) == "scripts/" &&
          path.substr(path.size() - 4) == ".pex")
        scripts.emplace(std::string(path), i);
    });

  NativeRegistry natives;
  rx::script::skyrim::RegisterSkyrimNatives(natives, nullptr);
  VirtualMachine vm(&natives);

  int parse_fail = 0, load_fail = 0, loaded = 0;
  std::vector<std::string> types;
  // type -> straight-line, no-arg functions in its default state, for --exec.
  std::map<std::string, std::vector<std::string>> safe_fns;
  for (const auto& [path, idx] : scripts) {
    auto blob = providers[idx]->Read(path);
    if (!blob) continue;
    PexFile pex;
    if (!ParsePex(ByteSpan(blob->data(), blob->size()), &pex) || pex.objects.empty()) {
      ++parse_fail;
      continue;
    }
    if (exec) {
      const Object& obj = pex.objects[0];
      std::string cls = pex.Str(obj.name);
      for (const State& st : obj.states) {
        if (!pex.Str(st.name).empty()) continue;  // default state only
        for (const NamedFunction& nf : st.functions)
          if (IsStraightLine(nf.function)) safe_fns[cls].push_back(pex.Str(nf.name));
      }
    }
    std::string type = vm.AddScript(std::move(pex));
    if (type.empty()) {
      ++load_fail;
      continue;
    }
    ++loaded;
    types.push_back(type);
  }

  // Instantiate a strided sample; a created, live instance proves member seeding
  // and the inheritance walk hold over real shipped class hierarchies.
  int created = 0, dead = 0;
  for (size_t i = 0; i < types.size(); i += stride) {
    ObjectRef inst = vm.CreateInstance(types[i]);
    if (vm.IsAlive(inst)) ++created; else ++dead;
  }

  std::printf("scripts=%zu loaded=%d parse_fail=%d load_fail=%d | instantiated=%d dead=%d "
              "(stride %d)\n",
              scripts.size(), loaded, parse_fail, load_fail, created, dead, stride);

  // Execute the safe bytecode functions on a fresh instance each, driving the
  // interpreter over real compiled code. A crash here would be a real opcode
  // bug; completing clean validates the opcode handlers against the corpus.
  if (exec) {
    int executed = 0;
    for (const auto& [cls, fns] : safe_fns) {
      ObjectRef inst = vm.CreateInstance(cls);
      if (!vm.IsAlive(inst)) continue;
      for (const std::string& fn : fns) {
        vm.Call(inst, fn, {});
        ++executed;
      }
    }
    std::printf("executed %d straight-line functions over real bytecode (no crash)\n", executed);
  }
  return (parse_fail == 0 && load_fail == 0 && dead == 0) ? 0 : 1;
}
