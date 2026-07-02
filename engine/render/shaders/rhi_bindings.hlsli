// Dual-target binding helpers. The engine's HLSL compiles to SPIR-V (Vulkan)
// and DXIL (D3D12) from the same source: [[vk::binding(N, S)]] drives the
// SPIR-V descriptor assignment, the sibling `: register(<class>N, spaceS)`
// annotation drives DXIL. Push-constant blocks have no DXIL equivalent, so
// PUSH_CONSTANTS lowers to a ConstantBuffer in register(b999, space0) that the
// d3d12 backend feeds through root constants (or a root CBV ring for blocks
// over the root-signature budget). dxc defines __spirv__ only when -spirv is
// passed, which keeps the Vulkan path byte-identical.
#ifndef RECREATION_RHI_BINDINGS_HLSLI_
#define RECREATION_RHI_BINDINGS_HLSLI_

#ifdef __spirv__
#define PUSH_CONSTANTS(T, name) [[vk::push_constant]] T name
#else
#define PUSH_CONSTANTS(T, name) ConstantBuffer<T> name : register(b999, space0)
#endif

#endif  // RECREATION_RHI_BINDINGS_HLSLI_
