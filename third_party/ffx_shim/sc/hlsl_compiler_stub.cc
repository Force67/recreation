// Link-level stub for the HLSL (DXC/FXC) compiler classes. Both the Linux and
// the native-Windows ffx_sc builds only ever drive glslang; selecting an HLSL
// compiler fails at runtime. (Compiled on every platform so the GLSL build
// never needs a real DXC/FXC toolchain.)
#include "hlsl_compiler.h"

#include <stdexcept>

HLSLCompiler::HLSLCompiler(Backend backend, const std::string&, const std::string& shaderPath,
                           const std::string& shaderName, const std::string& shaderFileName,
                           const std::string& outputPath, bool disableLogs, bool debugCompile)
    : ICompiler(shaderPath, shaderName, shaderFileName, outputPath, disableLogs, debugCompile),
      m_backend(backend),
      m_DxcCreateInstanceFunc(nullptr),
      m_FxcD3DCompile(nullptr),
      m_FxcD3DGetBlobPart(nullptr),
      m_FxcD3DReflect(nullptr),
      m_DllHandle(nullptr) {
  throw std::runtime_error("HLSL compilation is not supported in this ffx_sc build (glslang/GLSL only)");
}

HLSLCompiler::~HLSLCompiler() = default;

bool HLSLCompiler::Compile(Permutation&, const std::vector<std::string>&, std::mutex&) {
  return false;
}

bool HLSLCompiler::ExtractReflectionData(Permutation&) { return false; }

void HLSLCompiler::WriteBinaryHeaderReflectionData(FILE*, const Permutation&, std::mutex&) {}

void HLSLCompiler::WritePermutationHeaderReflectionStructMembers(FILE*) {}

void HLSLCompiler::WritePermutationHeaderReflectionData(FILE*, const Permutation&) {}

uint8_t* HLSLDxcShaderBinary::BufferPointer() { return nullptr; }
size_t HLSLDxcShaderBinary::BufferSize() { return 0; }
uint8_t* HLSLFxcShaderBinary::BufferPointer() { return nullptr; }
size_t HLSLFxcShaderBinary::BufferSize() { return 0; }
