// Minimal <dxcapi.h> for the native-Windows ffx_sc build. The DXC SDK header is
// not part of the Windows SDK, and this build only ever drives glslang (the
// HLSL/DXC path is stubbed in hlsl_compiler_stub.cc), so forward declarations
// are enough for the HLSLCompiler member declarations in hlsl_compiler.h to
// parse. Real <Windows.h>/<d3dcompiler.h>/<d3d12shader.h> still resolve from the
// Windows SDK because this directory is the only shim dir on the Windows include
// path.
#ifndef RECREATION_FFX_SHIM_WIN_DXCAPI_H_
#define RECREATION_FFX_SHIM_WIN_DXCAPI_H_

struct IDxcBlob;
struct IDxcResult;
struct IDxcUtils;
struct IDxcCompiler3;
struct IDxcIncludeHandler;
using DxcCreateInstanceProc = void*;

#endif  // RECREATION_FFX_SHIM_WIN_DXCAPI_H_
