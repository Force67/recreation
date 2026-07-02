// D3D12 command recording. Legacy resource states throughout (vkd3d 2.0 has
// no enhanced barriers): texture states are tracked per subresource on the
// device records, buffer states per command list (buffers decay to COMMON at
// every ExecuteCommandLists boundary), and the coarse rhi barrier scopes
// lower to global UAV barriers plus the lazy per-resource transitions.

#include "render/d3d12/d3d12_backend.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"

namespace rec::render::d3d12 {
namespace {

// Tightly packed row footprint for buffer<->texture copies. For BC formats a
// "row" is a block row (FormatTexelBytes returns bytes per block).
struct RowInfo {
  u32 row_bytes = 0;
  u32 row_count = 0;
};

RowInfo RowInfoOf(Format format, u32 width, u32 height) {
  bool block = format >= Format::kBC1RgbUnorm && format <= Format::kBC7Srgb;
  u32 texel = FormatTexelBytes(format);
  if (block) {
    return {((width + 3) / 4) * texel, (height + 3) / 4};
  }
  return {width * texel, height};
}

constexpr u32 kRowPitchAlign = 256;   // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
constexpr u32 kPlacementAlign = 512;  // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT

}  // namespace

void D3D12CommandList::OnBeginRecording() {
  bound_ = nullptr;
  std::memset(push_shadow_, 0, sizeof(push_shadow_));
  for (PendingVertexBuffer& vb : pending_vb_) vb = {};
  vb_dirty_ = false;
  buffer_states_.clear();
  timestamp_max_.clear();
}

void D3D12CommandList::OnEndRecording() {
  for (auto entry : timestamp_max_) {
    auto* pool = reinterpret_cast<TimestampPoolRecord*>(entry.key);
    list_->ResolveQueryData(pool->heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, entry.value,
                            pool->readback, 0);
  }
  timestamp_max_.clear();
}

void D3D12CommandList::RequireBufferState(BufferRecord* buffer, D3D12_RESOURCE_STATES state) {
  if (!buffer || buffer->fixed_state) return;
  u64 key = reinterpret_cast<u64>(buffer);
  u32* tracked = buffer_states_.find(key);
  D3D12_RESOURCE_STATES current =
      tracked ? static_cast<D3D12_RESOURCE_STATES>(*tracked) : D3D12_RESOURCE_STATE_COMMON;
  if (current == state) return;
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = buffer->resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = current;
  barrier.Transition.StateAfter = state;
  list_->ResourceBarrier(1, &barrier);
  if (tracked) {
    *tracked = static_cast<u32>(state);
  } else {
    buffer_states_.insert(key, static_cast<u32>(state));
  }
}

void D3D12CommandList::RequireTextureState(TextureRecord* texture, u32 base_mip, u32 mip_count,
                                           D3D12_RESOURCE_STATES state) {
  if (!texture) return;
  if (mip_count == 0) mip_count = texture->mip_levels - base_mip;
  D3D12_RESOURCE_BARRIER barriers[16];
  u32 count = 0;
  auto flush = [&] {
    if (count) list_->ResourceBarrier(count, barriers);
    count = 0;
  };
  for (u32 layer = 0; layer < texture->array_layers; ++layer) {
    for (u32 mip = base_mip; mip < base_mip + mip_count; ++mip) {
      u32 sub = mip + layer * texture->mip_levels;
      D3D12_RESOURCE_STATES current = texture->sub_states[sub];
      if (current == state) continue;
      D3D12_RESOURCE_BARRIER& b = barriers[count++];
      b = {};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = texture->resource;
      b.Transition.Subresource = sub;
      b.Transition.StateBefore = current;
      b.Transition.StateAfter = state;
      texture->sub_states[sub] = state;
      if (count == 16) flush();
    }
  }
  flush();
}

// --- binding ---

void D3D12CommandList::BindPipeline(PipelineHandle pipeline) {
  bound_ = Rec(pipeline);
  if (!bound_) return;
  list_->SetPipelineState(bound_->pso);
  if (bound_->compute) {
    list_->SetComputeRootSignature(bound_->root);
  } else {
    list_->SetGraphicsRootSignature(bound_->root);
    list_->IASetPrimitiveTopology(bound_->topology);
    vb_dirty_ = true;  // strides come from the pipeline
  }
}

void D3D12CommandList::BindSet(u32 set_index, BindingSetHandle set) {
  if (!bound_ || set_index >= bound_->sets.size()) return;
  const BindingSetRecord* record = Rec(set);
  const PipelineRecord::SetParams& params = bound_->sets[set_index];
  if (params.view_param >= 0 && record->view_start != ~0u) {
    if (bound_->compute) {
      list_->SetComputeRootDescriptorTable(params.view_param,
                                           device_.ViewGpu(record->view_start));
    } else {
      list_->SetGraphicsRootDescriptorTable(params.view_param,
                                            device_.ViewGpu(record->view_start));
    }
  }
  if (params.sampler_param >= 0 && record->sampler_start != ~0u) {
    if (bound_->compute) {
      list_->SetComputeRootDescriptorTable(params.sampler_param,
                                           device_.SamplerGpu(record->sampler_start));
    } else {
      list_->SetGraphicsRootDescriptorTable(params.sampler_param,
                                            device_.SamplerGpu(record->sampler_start));
    }
  }
}

void D3D12CommandList::BindTransient(u32 set_index, std::span<const BindingItem> items) {
  if (!bound_ || set_index >= bound_->sets.size()) return;
  const PipelineRecord::SetParams& params = bound_->sets[set_index];
  SetLayout* layout = params.layout;
  if (!layout) return;

  if (params.view_param >= 0 && layout->view_count > 0) {
    u32 table = device_.AllocTransientViews(ring_, layout->view_count);
    device_.WriteNullViewDescriptors(*layout, table);
    for (const BindingItem& item : items) {
      if (item.type == BindingType::kSampler) continue;
      const SetLayout::Slot* slot = layout->Find(item.slot);
      if (!slot) continue;
      device_.WriteViewDescriptor(*slot, item, table);
      // Legacy-state upkeep for descriptor-bound buffers. Textures are the
      // render graph's business (explicit rhi transitions).
      if (item.type == BindingType::kUniformBuffer) {
        RequireBufferState(Rec(item.buffer), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
      } else if (item.type == BindingType::kStorageBuffer) {
        BufferRecord* buffer = Rec(item.buffer);
        RequireBufferState(buffer, buffer->allows_uav
                                       ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                       : (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
      }
    }
    if (bound_->compute) {
      list_->SetComputeRootDescriptorTable(params.view_param, device_.ViewGpu(table));
    } else {
      list_->SetGraphicsRootDescriptorTable(params.view_param, device_.ViewGpu(table));
    }
  }

  if (params.sampler_param >= 0 && layout->sampler_count > 0) {
    // Sampler tables are deduplicated and cached forever (the shader-visible
    // sampler heap caps at 2048 descriptors).
    u64 handles[16] = {};
    for (const BindingItem& item : items) {
      if (item.type != BindingType::kSampler &&
          item.type != BindingType::kCombinedTextureSampler) {
        continue;
      }
      const SetLayout::Slot* slot = layout->Find(item.slot);
      if (!slot || slot->sampler_offset == ~0u) continue;
      u32 offset = slot->sampler_offset + item.array_index;
      if (offset < 16) handles[offset] = item.sampler.value;
    }
    u32 table = device_.GetSamplerTable(handles, layout->sampler_count);
    if (bound_->compute) {
      list_->SetComputeRootDescriptorTable(params.sampler_param, device_.SamplerGpu(table));
    } else {
      list_->SetGraphicsRootDescriptorTable(params.sampler_param, device_.SamplerGpu(table));
    }
  }
}

void D3D12CommandList::PushConstants(const void* data, u32 size, u32 offset) {
  if (!bound_ || bound_->push_param < 0) return;
  if (offset + size > sizeof(push_shadow_)) return;
  std::memcpy(push_shadow_ + offset, data, size);
  u32 block = std::min<u32>(bound_->push_size, sizeof(push_shadow_));
  if (bound_->push_root_constants) {
    if (bound_->compute) {
      list_->SetComputeRoot32BitConstants(bound_->push_param, block / 4, push_shadow_, 0);
    } else {
      list_->SetGraphicsRoot32BitConstants(bound_->push_param, block / 4, push_shadow_, 0);
    }
  } else {
    SetPushRootCbv();
  }
  // Push-block BDA convention: blocks big enough to carry the skinned bone
  // palette address (u64 at byte 128) feed it to the t998 root SRV. Shaders
  // that do not declare rec_bone_palette leave the parameter unused.
  if (bound_->bda_param >= 0 && bound_->push_size >= 136 && offset <= 128 &&
      offset + size >= 136) {
    u64 address = 0;
    std::memcpy(&address, push_shadow_ + 128, sizeof(address));
    if (address != 0) {
      if (bound_->compute) {
        list_->SetComputeRootShaderResourceView(bound_->bda_param, address);
      } else {
        list_->SetGraphicsRootShaderResourceView(bound_->bda_param, address);
      }
    }
  }
}

void D3D12CommandList::SetPushRootCbv() {
  void* cpu = nullptr;
  u32 block = std::min<u32>(bound_->push_size, sizeof(push_shadow_));
  u64 va = device_.AllocPushSlice(ring_, block, &cpu);
  std::memcpy(cpu, push_shadow_, block);
  if (bound_->compute) {
    list_->SetComputeRootConstantBufferView(bound_->push_param, va);
  } else {
    list_->SetGraphicsRootConstantBufferView(bound_->push_param, va);
  }
}

// --- compute ---

void D3D12CommandList::Dispatch(u32 x, u32 y, u32 z) {
  if (!bound_) return;  // pipeline creation may have failed (dxil-less shader)
  list_->Dispatch(x, y, z);
}

// --- raster ---

void D3D12CommandList::BeginRendering(const RenderingInfo& info) {
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
  u32 rtv_count = 0;
  for (const ColorAttachment& color : info.colors) {
    rtvs[rtv_count++] = device_.rtv_pool().Cpu(device_.EnsureRtv(Rec(color.view)));
  }
  D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
  bool has_depth = info.depth && info.depth->view;
  if (has_depth) {
    dsv = device_.dsv_pool().Cpu(device_.EnsureDsv(Rec(info.depth->view)));
  }
  list_->OMSetRenderTargets(rtv_count, rtv_count ? rtvs : nullptr, FALSE,
                            has_depth ? &dsv : nullptr);

  for (size_t i = 0; i < info.colors.size(); ++i) {
    if (info.colors[i].load == LoadOp::kClear) {
      list_->ClearRenderTargetView(rtvs[i], info.colors[i].clear, 0, nullptr);
    }
  }
  if (has_depth && info.depth->load == LoadOp::kClear) {
    list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, info.depth->clear, 0, 0, nullptr);
  }

  SetViewport(0, 0, static_cast<f32>(info.extent.width), static_cast<f32>(info.extent.height));
  SetScissor(0, 0, info.extent.width, info.extent.height);
}

void D3D12CommandList::EndRendering() {}

void D3D12CommandList::SetViewport(f32 x, f32 y, f32 width, f32 height) {
  // Inverted viewport (negative height, legal in D3D12): cancels the D3D/VK
  // NDC y-flip so the shared Vulkan-tuned HLSL renders identically. Through
  // vkd3d this exactly restores the plain Vulkan viewport.
  D3D12_VIEWPORT viewport{x, y + height, width, -height, 0.0f, 1.0f};
  list_->RSSetViewports(1, &viewport);
}

void D3D12CommandList::SetScissor(i32 x, i32 y, u32 width, u32 height) {
  D3D12_RECT rect{x, y, x + static_cast<i32>(width), y + static_cast<i32>(height)};
  list_->RSSetScissorRects(1, &rect);
}

void D3D12CommandList::BindVertexBuffer(u32 binding, const GpuBuffer& buffer, u64 offset) {
  if (binding >= 8) return;
  BufferRecord* record = Rec(buffer.handle);
  RequireBufferState(record, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
  pending_vb_[binding] = {record->gpu_va + offset, buffer.size - offset, true};
  vb_dirty_ = true;
}

void D3D12CommandList::FlushVertexBuffers() {
  if (!vb_dirty_ || !bound_ || bound_->compute) return;
  for (u32 i = 0; i < 8; ++i) {
    if (!pending_vb_[i].set) continue;
    u32 stride = i < bound_->vertex_strides.size() ? bound_->vertex_strides[i] : 0;
    D3D12_VERTEX_BUFFER_VIEW view{pending_vb_[i].va, static_cast<u32>(pending_vb_[i].size),
                                  stride};
    list_->IASetVertexBuffers(i, 1, &view);
  }
  vb_dirty_ = false;
}

void D3D12CommandList::BindIndexBuffer(const GpuBuffer& buffer, u64 offset, IndexType type) {
  BufferRecord* record = Rec(buffer.handle);
  RequireBufferState(record, D3D12_RESOURCE_STATE_INDEX_BUFFER);
  D3D12_INDEX_BUFFER_VIEW view{record->gpu_va + offset, static_cast<u32>(buffer.size - offset),
                               type == IndexType::kUint16 ? DXGI_FORMAT_R16_UINT
                                                          : DXGI_FORMAT_R32_UINT};
  list_->IASetIndexBuffer(&view);
}

void D3D12CommandList::Draw(u32 vertex_count, u32 instance_count, u32 first_vertex,
                            u32 first_instance) {
  if (!bound_) return;
  FlushVertexBuffers();
  list_->DrawInstanced(vertex_count, instance_count, first_vertex, first_instance);
}

void D3D12CommandList::DrawIndexed(u32 index_count, u32 instance_count, u32 first_index,
                                   i32 vertex_offset, u32 first_instance) {
  if (!bound_) return;
  FlushVertexBuffers();
  list_->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset,
                              first_instance);
}

void D3D12CommandList::DrawIndexedIndirect(const GpuBuffer& args, u64 offset, u32 draw_count,
                                           u32 stride) {
  if (!bound_) return;
  FlushVertexBuffers();
  BufferRecord* record = Rec(args.handle);
  RequireBufferState(record, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
  ID3D12CommandSignature* signature = device_.GetDrawIndexedSignature(stride);
  if (!signature) return;
  list_->ExecuteIndirect(signature, draw_count, record->resource, offset, nullptr, 0);
}

void D3D12CommandList::DrawMeshTasks(u32 x, u32 y, u32 z) {
  if (!bound_) return;
  ID3D12GraphicsCommandList6* list6 = nullptr;
  if (SUCCEEDED(list_->QueryInterface(IID_ID3D12GraphicsCommandList6,
                                      reinterpret_cast<void**>(&list6)))) {
    list6->DispatchMesh(x, y, z);
    list6->Release();
  }
}

// --- synchronization ---

void D3D12CommandList::TextureBarriers(std::span<const TextureBarrier> barriers) {
  for (const TextureBarrier& barrier : barriers) {
    TextureRecord* texture = Rec(barrier.texture);
    if (!texture) continue;
    D3D12_RESOURCE_STATES after = ToResourceStates(barrier.after);
    // General->general is a UAV hazard, not a state change.
    if (barrier.before == ResourceState::kGeneral && barrier.after == ResourceState::kGeneral) {
      D3D12_RESOURCE_BARRIER uav = {};
      uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      uav.UAV.pResource = texture->resource;
      list_->ResourceBarrier(1, &uav);
      continue;
    }
    // The tracked state is authoritative (kUndefined means "whatever it was").
    RequireTextureState(texture, barrier.base_mip, barrier.mip_count, after);
  }
}

void D3D12CommandList::MemoryBarrier(BarrierScope src, BarrierScope dst) {
  (void)src;
  (void)dst;
  // Coarse scopes lower to a global UAV barrier; the remaining buffer hazards
  // (copy <-> shader, indirect args) are covered by the lazy per-buffer
  // transitions at use sites.
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  list_->ResourceBarrier(1, &barrier);
}

// --- transfer ---

void D3D12CommandList::CopyBufferToTexture(const GpuBuffer& src, const GpuImage& dst,
                                           std::span<const BufferTextureCopy> regions) {
  TextureRecord* texture = Rec(dst.handle);
  BufferRecord* buffer = Rec(src.handle);

  // D3D12 buffer<->texture copies need 256-byte row pitch and 512-byte
  // placement; the engine packs tightly. Misaligned uploads bounce through an
  // internal aligned staging buffer filled from the (host visible) source.
  bool aligned = true;
  for (const BufferTextureCopy& region : regions) {
    u32 width = region.extent.width ? region.extent.width
                                    : std::max(dst.extent.width >> region.mip, 1u);
    u32 height = region.extent.height ? region.extent.height
                                      : std::max(dst.extent.height >> region.mip, 1u);
    RowInfo rows = RowInfoOf(dst.format, width, height);
    if ((rows.row_bytes % kRowPitchAlign) != 0 || (region.buffer_offset % kPlacementAlign) != 0) {
      aligned = false;
    }
  }

  ID3D12Resource* source = buffer->resource;
  base::Vector<u64> staged_offsets(regions.size());
  ID3D12Resource* staging = nullptr;
  if (!aligned) {
    const u8* mapped = nullptr;
    void* raw = nullptr;
    if (SUCCEEDED(buffer->resource->Map(0, nullptr, &raw))) mapped = static_cast<const u8*>(raw);
    if (!mapped) {
      REC_ERROR("d3d12: misaligned texture upload from an unmapped buffer, dropping");
      return;
    }
    u64 total = 0;
    for (size_t i = 0; i < regions.size(); ++i) {
      const BufferTextureCopy& region = regions[i];
      u32 width = region.extent.width ? region.extent.width
                                      : std::max(dst.extent.width >> region.mip, 1u);
      u32 height = region.extent.height ? region.extent.height
                                        : std::max(dst.extent.height >> region.mip, 1u);
      RowInfo rows = RowInfoOf(dst.format, width, height);
      u64 pitch = (rows.row_bytes + kRowPitchAlign - 1) & ~static_cast<u64>(kRowPitchAlign - 1);
      total = (total + kPlacementAlign - 1) & ~static_cast<u64>(kPlacementAlign - 1);
      staged_offsets[i] = total;
      total += pitch * rows.row_count;
    }

    D3D12_HEAP_PROPERTIES upload = {};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = total;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device_.device()->CreateCommittedResource(
            &upload, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_ID3D12Resource, reinterpret_cast<void**>(&staging)))) {
      REC_ERROR("d3d12: upload staging allocation failed ({} bytes)", total);
      return;
    }
    void* staging_raw = nullptr;
    staging->Map(0, nullptr, &staging_raw);
    u8* out = static_cast<u8*>(staging_raw);
    for (size_t i = 0; i < regions.size(); ++i) {
      const BufferTextureCopy& region = regions[i];
      u32 width = region.extent.width ? region.extent.width
                                      : std::max(dst.extent.width >> region.mip, 1u);
      u32 height = region.extent.height ? region.extent.height
                                        : std::max(dst.extent.height >> region.mip, 1u);
      RowInfo rows = RowInfoOf(dst.format, width, height);
      u64 pitch = (rows.row_bytes + kRowPitchAlign - 1) & ~static_cast<u64>(kRowPitchAlign - 1);
      for (u32 row = 0; row < rows.row_count; ++row) {
        std::memcpy(out + staged_offsets[i] + row * pitch,
                    mapped + region.buffer_offset + static_cast<u64>(row) * rows.row_bytes,
                    rows.row_bytes);
      }
    }
    staging->Unmap(0, nullptr);
    source = staging;
    device_.DeferRelease(ring_, staging);
  } else {
    RequireBufferState(buffer, D3D12_RESOURCE_STATE_COPY_SOURCE);
  }

  for (size_t i = 0; i < regions.size(); ++i) {
    const BufferTextureCopy& region = regions[i];
    u32 width = region.extent.width ? region.extent.width
                                    : std::max(dst.extent.width >> region.mip, 1u);
    u32 height = region.extent.height ? region.extent.height
                                      : std::max(dst.extent.height >> region.mip, 1u);
    RowInfo rows = RowInfoOf(dst.format, width, height);
    u64 pitch = aligned
                    ? rows.row_bytes
                    : ((rows.row_bytes + kRowPitchAlign - 1) & ~static_cast<u64>(kRowPitchAlign - 1));

    D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
    dst_loc.pResource = texture->resource;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = region.mip + region.array_layer * texture->mip_levels;
    D3D12_TEXTURE_COPY_LOCATION src_loc = {};
    src_loc.pResource = source;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint.Offset = aligned ? region.buffer_offset : staged_offsets[i];
    src_loc.PlacedFootprint.Footprint.Format = texture->format;
    src_loc.PlacedFootprint.Footprint.Width = width;
    src_loc.PlacedFootprint.Footprint.Height = height;
    src_loc.PlacedFootprint.Footprint.Depth = 1;
    src_loc.PlacedFootprint.Footprint.RowPitch = static_cast<u32>(pitch);
    list_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
  }
}

void D3D12CommandList::CopyTextureToBuffer(const GpuImage& src, const GpuBuffer& dst,
                                           const BufferTextureCopy& region) {
  TextureRecord* texture = Rec(src.handle);
  BufferRecord* buffer = Rec(dst.handle);
  u32 width = region.extent.width ? region.extent.width
                                  : std::max(src.extent.width >> region.mip, 1u);
  u32 height = region.extent.height ? region.extent.height
                                    : std::max(src.extent.height >> region.mip, 1u);
  RowInfo rows = RowInfoOf(src.format, width, height);

  bool aligned =
      (rows.row_bytes % kRowPitchAlign) == 0 && (region.buffer_offset % kPlacementAlign) == 0;

  D3D12_TEXTURE_COPY_LOCATION src_loc = {};
  src_loc.pResource = texture->resource;
  src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src_loc.SubresourceIndex = region.mip + region.array_layer * texture->mip_levels;

  if (aligned) {
    RequireBufferState(buffer, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
    dst_loc.pResource = buffer->resource;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst_loc.PlacedFootprint.Offset = region.buffer_offset;
    dst_loc.PlacedFootprint.Footprint.Format = texture->format;
    dst_loc.PlacedFootprint.Footprint.Width = width;
    dst_loc.PlacedFootprint.Footprint.Height = height;
    dst_loc.PlacedFootprint.Footprint.Depth = 1;
    dst_loc.PlacedFootprint.Footprint.RowPitch = rows.row_bytes;
    list_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
    return;
  }

  // Misaligned readback: copy into an aligned intermediate, then repack the
  // rows GPU-side (buffer copies have no alignment constraints).
  u64 pitch = (rows.row_bytes + kRowPitchAlign - 1) & ~static_cast<u64>(kRowPitchAlign - 1);
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = pitch * rows.row_count;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* intermediate = nullptr;
  if (FAILED(device_.device()->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_ID3D12Resource, reinterpret_cast<void**>(&intermediate)))) {
    REC_ERROR("d3d12: readback staging allocation failed");
    return;
  }
  device_.DeferRelease(ring_, intermediate);

  D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
  dst_loc.pResource = intermediate;
  dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst_loc.PlacedFootprint.Footprint.Format = texture->format;
  dst_loc.PlacedFootprint.Footprint.Width = width;
  dst_loc.PlacedFootprint.Footprint.Height = height;
  dst_loc.PlacedFootprint.Footprint.Depth = 1;
  dst_loc.PlacedFootprint.Footprint.RowPitch = static_cast<u32>(pitch);
  list_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = intermediate;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  list_->ResourceBarrier(1, &barrier);

  RequireBufferState(buffer, D3D12_RESOURCE_STATE_COPY_DEST);
  for (u32 row = 0; row < rows.row_count; ++row) {
    list_->CopyBufferRegion(buffer->resource,
                            region.buffer_offset + static_cast<u64>(row) * rows.row_bytes,
                            intermediate, row * pitch, rows.row_bytes);
  }
}

void D3D12CommandList::CopyBuffer(const GpuBuffer& src, u64 src_offset, const GpuBuffer& dst,
                                  u64 dst_offset, u64 size) {
  RequireBufferState(Rec(src.handle), D3D12_RESOURCE_STATE_COPY_SOURCE);
  RequireBufferState(Rec(dst.handle), D3D12_RESOURCE_STATE_COPY_DEST);
  list_->CopyBufferRegion(Rec(dst.handle)->resource, dst_offset, Rec(src.handle)->resource,
                          src_offset, size);
}

void D3D12CommandList::BlitMip(const GpuImage& image, u32 src_mip, Extent2D src_extent,
                               u32 dst_mip, Extent2D dst_extent) {
  (void)src_extent;
  TextureRecord* texture = Rec(image.handle);
  PipelineHandle blit = device_.GetBlitPipeline(image.format);
  if (!blit) return;

  // Caller keeps src mip in kCopySrc / dst mip in kCopyDst; run the draw and
  // restore so the caller's mip-chain state walk stays valid.
  RequireTextureState(texture, src_mip, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  RequireTextureState(texture, dst_mip, 1, D3D12_RESOURCE_STATE_RENDER_TARGET);

  TextureView src_view = device_.CreateMipView(image, src_mip);
  TextureView dst_view = device_.CreateMipView(image, dst_mip);

  ColorAttachment color;
  color.view = dst_view;
  color.load = LoadOp::kDontCare;
  RenderingInfo info;
  info.extent = dst_extent;
  info.colors = std::span<const ColorAttachment>(&color, 1);
  BeginRendering(info);
  BindPipeline(blit);
  BindingItem blit_src = Bind::Combined(0, src_view, device_.blit_sampler());
  BindTransient(0, std::span<const BindingItem>(&blit_src, 1));
  Draw(3, 1, 0, 0);
  EndRendering();

  // Both CPU descriptors were consumed at record time (OMSetRenderTargets
  // snapshots RTVs; BindTransient copied the SRV into the shader-visible
  // ring), so the transient views can go immediately.
  device_.DestroyView(src_view);
  device_.DestroyView(dst_view);

  RequireTextureState(texture, src_mip, 1, D3D12_RESOURCE_STATE_COPY_SOURCE);
  RequireTextureState(texture, dst_mip, 1, D3D12_RESOURCE_STATE_COPY_DEST);
}

void D3D12CommandList::ClearColor(const GpuImage& image, const f32 color[4]) {
  TextureRecord* texture = Rec(image.handle);
  bool has_rtv = false;
  {
    // Rendertarget-capable images clear through RTVs; storage images through
    // a UAV clear. (Either way the caller holds the image in kCopyDst.)
    D3D12_RESOURCE_DESC desc = texture->resource->GetDesc();
    has_rtv = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
  }
  if (has_rtv) {
    RequireTextureState(texture, 0, texture->mip_levels, D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (u32 mip = 0; mip < texture->mip_levels; ++mip) {
      TextureView view = device_.CreateMipView(image, mip);
      u32 rtv = device_.EnsureRtv(Rec(view));
      list_->ClearRenderTargetView(device_.rtv_pool().Cpu(rtv), color, 0, nullptr);
      device_.DestroyView(view);
    }
    RequireTextureState(texture, 0, texture->mip_levels, D3D12_RESOURCE_STATE_COPY_DEST);
    return;
  }
  if (texture->usage & kTextureUsageStorage) {
    RequireTextureState(texture, 0, texture->mip_levels, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (u32 mip = 0; mip < texture->mip_levels; ++mip) {
      TextureView view = device_.CreateMipView(image, mip);
      u32 uav_cpu = device_.EnsureUav(Rec(view));
      u32 table = device_.AllocTransientViews(ring_, 1);
      device_.device()->CopyDescriptorsSimple(1, device_.ViewCpu(table),
                                              device_.srv_pool().Cpu(uav_cpu),
                                              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      list_->ClearUnorderedAccessViewFloat(device_.ViewGpu(table),
                                           device_.srv_pool().Cpu(uav_cpu), texture->resource,
                                           color, 0, nullptr);
      device_.DestroyView(view);
    }
    RequireTextureState(texture, 0, texture->mip_levels, D3D12_RESOURCE_STATE_COPY_DEST);
    return;
  }
  REC_WARN("d3d12: ClearColor on an image with neither RTV nor UAV capability");
}

void D3D12CommandList::ClearDepth(const GpuImage& image, f32 depth) {
  TextureRecord* texture = Rec(image.handle);
  RequireTextureState(texture, 0, texture->mip_levels, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  for (u32 mip = 0; mip < texture->mip_levels; ++mip) {
    TextureView view = device_.CreateMipView(image, mip);
    u32 dsv = device_.EnsureDsv(Rec(view));
    list_->ClearDepthStencilView(device_.dsv_pool().Cpu(dsv), D3D12_CLEAR_FLAG_DEPTH, depth, 0, 0,
                                 nullptr);
    device_.DestroyView(view);
  }
  RequireTextureState(texture, 0, texture->mip_levels, D3D12_RESOURCE_STATE_COPY_DEST);
}

void D3D12CommandList::FillBuffer(const GpuBuffer& buffer, u64 offset, u64 size, u32 data) {
  BufferRecord* record = Rec(buffer.handle);
  if (!record->allows_uav) {
    REC_WARN("d3d12: FillBuffer on a non-UAV buffer, dropping");
    return;
  }
  RequireBufferState(record, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
  desc.Format = DXGI_FORMAT_R32_TYPELESS;
  desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  desc.Buffer.FirstElement = offset / 4;
  desc.Buffer.NumElements = static_cast<u32>(size / 4);
  desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  u32 cpu = device_.srv_pool().Alloc();
  device_.device()->CreateUnorderedAccessView(record->resource, nullptr, &desc,
                                              device_.srv_pool().Cpu(cpu));
  u32 table = device_.AllocTransientViews(ring_, 1);
  device_.device()->CopyDescriptorsSimple(1, device_.ViewCpu(table), device_.srv_pool().Cpu(cpu),
                                          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT values[4] = {data, data, data, data};
  list_->ClearUnorderedAccessViewUint(device_.ViewGpu(table), device_.srv_pool().Cpu(cpu),
                                      record->resource, values, 0, nullptr);
  device_.srv_pool().Free(cpu);  // consumed at record time by vkd3d/d3d12
}

// --- acceleration structures ---

void D3D12CommandList::BuildBlas(AccelStructHandle blas, const BlasBuildDesc& desc,
                                 const GpuBuffer& scratch, u64 scratch_offset) {
  ID3D12GraphicsCommandList4* list4 = nullptr;
  if (FAILED(list_->QueryInterface(IID_ID3D12GraphicsCommandList4,
                                   reinterpret_cast<void**>(&list4)))) {
    return;
  }
  base::Vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
  for (const AccelTriangles& t : desc.geometries) {
    D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = t.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE
                              : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    geometry.Triangles.VertexBuffer = {t.vertex_address, t.vertex_stride};
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.VertexCount = t.vertex_count;
    geometry.Triangles.IndexBuffer = t.index_address;
    geometry.Triangles.IndexCount = t.index_count;
    geometry.Triangles.IndexFormat =
        t.index_type == IndexType::kUint16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    geometries.push_back(geometry);
  }

  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
  build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
  build.Inputs.Flags = desc.fast_trace
                           ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
                           : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
  build.Inputs.NumDescs = static_cast<u32>(geometries.size());
  build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  build.Inputs.pGeometryDescs = geometries.data();
  build.DestAccelerationStructureData = Rec(blas)->address;
  build.ScratchAccelerationStructureData = scratch.address + scratch_offset;
  list4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
  list4->Release();
}

void D3D12CommandList::BuildTlas(AccelStructHandle tlas, const GpuBuffer& instances,
                                 u32 instance_count, const GpuBuffer& scratch) {
  ID3D12GraphicsCommandList4* list4 = nullptr;
  if (FAILED(list_->QueryInterface(IID_ID3D12GraphicsCommandList4,
                                   reinterpret_cast<void**>(&list4)))) {
    return;
  }
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
  build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
  build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
  build.Inputs.NumDescs = instance_count;
  build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  build.Inputs.InstanceDescs = instances.address;
  build.DestAccelerationStructureData = Rec(tlas)->address;
  u64 alignment = device_.caps().accel_scratch_alignment;
  build.ScratchAccelerationStructureData = (scratch.address + alignment - 1) & ~(alignment - 1);
  list4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
  list4->Release();
}

// --- profiling ---

void D3D12CommandList::ResetTimestamps(TimestampPoolHandle pool, u32 first, u32 count) {
  (void)pool;
  (void)first;
  (void)count;  // no reset in d3d12; resolve happens at OnEndRecording
}

void D3D12CommandList::WriteTimestamp(TimestampPoolHandle pool, u32 index, bool after_work) {
  (void)after_work;  // both edges are END queries in d3d12
  TimestampPoolRecord* record = Rec(pool);
  list_->EndQuery(record->heap, D3D12_QUERY_TYPE_TIMESTAMP, index);
  u64 key = reinterpret_cast<u64>(record);
  u32* max = timestamp_max_.find(key);
  if (max) {
    *max = std::max(*max, index + 1);
  } else {
    timestamp_max_.insert(key, index + 1);
  }
}

void D3D12CommandList::BeginDebugLabel(const char* name) { (void)name; }
void D3D12CommandList::EndDebugLabel() {}

}  // namespace rec::render::d3d12
