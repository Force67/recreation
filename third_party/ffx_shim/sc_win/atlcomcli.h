// Minimal CComPtr for the native-Windows ffx_sc build so it does not depend on
// the optional ATL component being installed in the toolchain. The GLSL-only
// build never instantiates a DXC/FXC COM object, so a trivial pointer wrapper
// is all the member declarations in hlsl_compiler.h need to parse.
#ifndef RECREATION_FFX_SHIM_WIN_ATLCOMCLI_H_
#define RECREATION_FFX_SHIM_WIN_ATLCOMCLI_H_

template <typename T>
class CComPtr {
 public:
  T* p = nullptr;
};

#endif  // RECREATION_FFX_SHIM_WIN_ATLCOMCLI_H_
