#include "asset/gltf_loader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "asset/asset_id.h"
#include "core/log.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include <stb_image.h>

namespace rec::asset {
namespace {

AssetId ScopedId(const std::string& path, const char* kind, size_t index) {
  return MakeAssetId(path + "#" + kind + std::to_string(index));
}

// Decodes one glTF image to rgba8, from an external file, a GLB buffer view
// or a base64 data uri.
bool DecodeImage(const cgltf_image* image, const std::filesystem::path& base_dir,
                 Texture* out) {
  stbi_uc* pixels = nullptr;
  int width = 0, height = 0, channels = 0;

  if (image->buffer_view) {
    const u8* bytes = static_cast<const u8*>(image->buffer_view->buffer->data) +
                      image->buffer_view->offset;
    pixels = stbi_load_from_memory(bytes, static_cast<int>(image->buffer_view->size), &width,
                                   &height, &channels, 4);
  } else if (image->uri) {
    if (std::strncmp(image->uri, "data:", 5) == 0) {
      const char* comma = std::strchr(image->uri, ',');
      if (!comma) return false;
      void* decoded = nullptr;
      cgltf_options options{};
      // Base64 length to byte count: every 4 chars carry 3 bytes.
      cgltf_size size = (std::strlen(comma + 1) / 4) * 3;
      if (cgltf_load_buffer_base64(&options, size, comma + 1, &decoded) != cgltf_result_success) {
        return false;
      }
      pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(decoded),
                                     static_cast<int>(size), &width, &height, &channels, 4);
      CGLTF_FREE(decoded);
    } else {
      char decoded_uri[1024];
      std::strncpy(decoded_uri, image->uri, sizeof(decoded_uri) - 1);
      decoded_uri[sizeof(decoded_uri) - 1] = 0;
      cgltf_decode_uri(decoded_uri);
      std::filesystem::path file = base_dir / decoded_uri;
      pixels = stbi_load(file.string().c_str(), &width, &height, &channels, 4);
    }
  }
  if (!pixels) return false;

  out->format = TextureFormat::kRgba8;
  out->width = static_cast<u32>(width);
  out->height = static_cast<u32>(height);
  out->mip_count = 1;
  out->data.resize(static_cast<size_t>(width) * height * 4);
  std::memcpy(out->data.data(), pixels, out->data.size());
  stbi_image_free(pixels);
  return true;
}

void ReadFloats(const cgltf_accessor* accessor, u32 components, base::Vector<f32>* out) {
  out->clear();
  if (!accessor) return;
  out->resize(accessor->count * components);
  // unpack_floats widens every component type and applies normalization.
  cgltf_accessor_unpack_floats(accessor, out->data(), accessor->count * components);
}

// Average uv-space tangents per vertex when the source has none. Not
// mikktspace, but enough for normal mapping on well behaved content.
void GenerateTangents(MeshLod* lod, u32 vertex_offset, u32 index_offset) {
  base::Vector<Vec3> tangents(lod->vertices.size() - vertex_offset);
  for (size_t i = index_offset; i + 2 < lod->indices.size(); i += 3) {
    Vertex& v0 = lod->vertices[lod->indices[i]];
    Vertex& v1 = lod->vertices[lod->indices[i + 1]];
    Vertex& v2 = lod->vertices[lod->indices[i + 2]];
    Vec3 e1{v1.position[0] - v0.position[0], v1.position[1] - v0.position[1],
            v1.position[2] - v0.position[2]};
    Vec3 e2{v2.position[0] - v0.position[0], v2.position[1] - v0.position[1],
            v2.position[2] - v0.position[2]};
    f32 du1 = v1.uv[0] - v0.uv[0], dv1 = v1.uv[1] - v0.uv[1];
    f32 du2 = v2.uv[0] - v0.uv[0], dv2 = v2.uv[1] - v0.uv[1];
    f32 det = du1 * dv2 - du2 * dv1;
    if (std::abs(det) < 1e-12f) continue;
    f32 inv = 1.0f / det;
    Vec3 tangent = (e1 * dv2 + e2 * -dv1) * inv;
    for (int corner = 0; corner < 3; ++corner) {
      u32 index = lod->indices[i + corner];
      if (index >= vertex_offset) tangents[index - vertex_offset] += tangent;
    }
  }
  for (size_t i = 0; i < tangents.size(); ++i) {
    Vertex& vertex = lod->vertices[vertex_offset + i];
    Vec3 n{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
    Vec3 t = tangents[i] - n * Dot(n, tangents[i]);
    if (Dot(t, t) < 1e-12f) {
      // Degenerate uvs; any frame orthogonal to n will do.
      t = std::abs(n.y) < 0.99f ? Cross(n, {0, 1, 0}) : Cross(n, {1, 0, 0});
    }
    t = Normalize(t);
    vertex.tangent[0] = t.x;
    vertex.tangent[1] = t.y;
    vertex.tangent[2] = t.z;
    vertex.tangent[3] = 1.0f;
  }
}

// Rotation quaternion from an orthonormalized basis, Shepperd's method.
void QuatFromMatrix(const f32 m[16], const Vec3& scale, f32 out[4]) {
  f32 r[9];  // row-major 3x3, normalized
  for (int col = 0; col < 3; ++col) {
    f32 axis_scale = col == 0 ? scale.x : (col == 1 ? scale.y : scale.z);
    f32 inv = axis_scale > 1e-12f ? 1.0f / axis_scale : 0.0f;
    for (int row = 0; row < 3; ++row) r[row * 3 + col] = m[col * 4 + row] * inv;
  }
  f32 trace = r[0] + r[4] + r[8];
  if (trace > 0) {
    f32 s = std::sqrt(trace + 1.0f) * 2;
    out[3] = 0.25f * s;
    out[0] = (r[7] - r[5]) / s;
    out[1] = (r[2] - r[6]) / s;
    out[2] = (r[3] - r[1]) / s;
  } else if (r[0] > r[4] && r[0] > r[8]) {
    f32 s = std::sqrt(1.0f + r[0] - r[4] - r[8]) * 2;
    out[3] = (r[7] - r[5]) / s;
    out[0] = 0.25f * s;
    out[1] = (r[1] + r[3]) / s;
    out[2] = (r[2] + r[6]) / s;
  } else if (r[4] > r[8]) {
    f32 s = std::sqrt(1.0f + r[4] - r[0] - r[8]) * 2;
    out[3] = (r[2] - r[6]) / s;
    out[0] = (r[1] + r[3]) / s;
    out[1] = 0.25f * s;
    out[2] = (r[5] + r[7]) / s;
  } else {
    f32 s = std::sqrt(1.0f + r[8] - r[0] - r[4]) * 2;
    out[3] = (r[3] - r[1]) / s;
    out[0] = (r[2] + r[6]) / s;
    out[1] = (r[5] + r[7]) / s;
    out[2] = 0.25f * s;
  }
}

}  // namespace

bool LoadGltfScene(const std::string& path, GltfScene* out) {
  cgltf_options options{};
  cgltf_data* data = nullptr;
  if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
    REC_ERROR("gltf parse failed: {}", path);
    return false;
  }
  if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
    REC_ERROR("gltf buffer load failed: {}", path);
    cgltf_free(data);
    return false;
  }
  std::filesystem::path base_dir = std::filesystem::path(path).parent_path();

  // Base color and emissive sample as srgb, data maps stay linear.
  base::Vector<bool> texture_srgb(data->textures_count);
  for (size_t i = 0; i < data->materials_count; ++i) {
    const cgltf_material& material = data->materials[i];
    if (material.has_pbr_metallic_roughness) {
      if (const cgltf_texture* t = material.pbr_metallic_roughness.base_color_texture.texture) {
        texture_srgb[static_cast<size_t>(t - data->textures)] = true;
      }
    }
    if (const cgltf_texture* t = material.emissive_texture.texture) {
      texture_srgb[static_cast<size_t>(t - data->textures)] = true;
    }
  }

  out->textures.resize(data->textures_count);
  for (size_t i = 0; i < data->textures_count; ++i) {
    Texture& texture = out->textures[i];
    texture.id = ScopedId(path, "tex", i);
    texture.is_srgb = texture_srgb[i];
    if (!data->textures[i].image || !DecodeImage(data->textures[i].image, base_dir, &texture)) {
      REC_WARN("gltf texture {} failed to decode", i);
      texture.id = {};
    }
  }

  auto texture_id = [&](const cgltf_texture* texture) -> AssetId {
    if (!texture) return {};
    return out->textures[static_cast<size_t>(texture - data->textures)].id;
  };

  out->materials.resize(data->materials_count);
  for (size_t i = 0; i < data->materials_count; ++i) {
    const cgltf_material& src = data->materials[i];
    Material& material = out->materials[i];
    material.id = ScopedId(path, "mat", i);
    if (src.has_pbr_metallic_roughness) {
      const auto& pbr = src.pbr_metallic_roughness;
      std::memcpy(material.base_color_factor, pbr.base_color_factor, sizeof(f32) * 4);
      material.metallic_factor = pbr.metallic_factor;
      material.roughness_factor = pbr.roughness_factor;
      material.base_color = texture_id(pbr.base_color_texture.texture);
      material.metallic_roughness = texture_id(pbr.metallic_roughness_texture.texture);
    }
    material.normal = texture_id(src.normal_texture.texture);
    material.emissive = texture_id(src.emissive_texture.texture);
    std::memcpy(material.emissive_factor, src.emissive_factor, sizeof(f32) * 3);
    material.alpha_cutoff = src.alpha_cutoff;
    material.alpha_mode = src.alpha_mode == cgltf_alpha_mode_opaque ? AlphaMode::kOpaque
                          : src.alpha_mode == cgltf_alpha_mode_mask ? AlphaMode::kMask
                                                                    : AlphaMode::kBlend;
    material.two_sided = src.double_sided;
  }

  out->meshes.resize(data->meshes_count);
  base::Vector<f32> scratch;
  for (size_t i = 0; i < data->meshes_count; ++i) {
    const cgltf_mesh& src = data->meshes[i];
    Mesh& mesh = out->meshes[i];
    mesh.id = ScopedId(path, "mesh", i);
    mesh.lods.resize(1);
    MeshLod& lod = mesh.lods[0];

    for (size_t p = 0; p < src.primitives_count; ++p) {
      const cgltf_primitive& primitive = src.primitives[p];
      if (primitive.type != cgltf_primitive_type_triangles) continue;

      const cgltf_accessor* position = nullptr;
      const cgltf_accessor* normal = nullptr;
      const cgltf_accessor* tangent = nullptr;
      const cgltf_accessor* uv = nullptr;
      const cgltf_accessor* color = nullptr;
      for (size_t a = 0; a < primitive.attributes_count; ++a) {
        const cgltf_attribute& attribute = primitive.attributes[a];
        if (attribute.index != 0) continue;
        switch (attribute.type) {
          case cgltf_attribute_type_position: position = attribute.data; break;
          case cgltf_attribute_type_normal: normal = attribute.data; break;
          case cgltf_attribute_type_tangent: tangent = attribute.data; break;
          case cgltf_attribute_type_texcoord: uv = attribute.data; break;
          case cgltf_attribute_type_color: color = attribute.data; break;
          default: break;
        }
      }
      if (!position) continue;

      u32 vertex_offset = static_cast<u32>(lod.vertices.size());
      u32 index_offset = static_cast<u32>(lod.indices.size());
      u32 vertex_count = static_cast<u32>(position->count);
      lod.vertices.resize(vertex_offset + vertex_count);

      ReadFloats(position, 3, &scratch);
      for (u32 v = 0; v < vertex_count; ++v) {
        std::memcpy(lod.vertices[vertex_offset + v].position, &scratch[v * 3], sizeof(f32) * 3);
      }
      if (normal && normal->count == vertex_count) {
        ReadFloats(normal, 3, &scratch);
        for (u32 v = 0; v < vertex_count; ++v) {
          std::memcpy(lod.vertices[vertex_offset + v].normal, &scratch[v * 3], sizeof(f32) * 3);
        }
      } else {
        for (u32 v = 0; v < vertex_count; ++v) {
          lod.vertices[vertex_offset + v].normal[1] = 1.0f;
        }
      }
      if (uv && uv->count == vertex_count) {
        ReadFloats(uv, 2, &scratch);
        for (u32 v = 0; v < vertex_count; ++v) {
          std::memcpy(lod.vertices[vertex_offset + v].uv, &scratch[v * 2], sizeof(f32) * 2);
        }
      }
      if (color && color->count == vertex_count) {
        u32 components = static_cast<u32>(cgltf_num_components(color->type));
        ReadFloats(color, components, &scratch);
        for (u32 v = 0; v < vertex_count; ++v) {
          f32 r = scratch[v * components + 0];
          f32 g = components > 1 ? scratch[v * components + 1] : r;
          f32 b = components > 2 ? scratch[v * components + 2] : r;
          f32 a = components > 3 ? scratch[v * components + 3] : 1.0f;
          auto to_u8 = [](f32 value) {
            return static_cast<u32>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
          };
          lod.vertices[vertex_offset + v].color =
              to_u8(r) | to_u8(g) << 8 | to_u8(b) << 16 | to_u8(a) << 24;
        }
      }

      if (primitive.indices) {
        u32 index_count = static_cast<u32>(primitive.indices->count);
        lod.indices.resize(index_offset + index_count);
        for (u32 n = 0; n < index_count; ++n) {
          lod.indices[index_offset + n] =
              vertex_offset + static_cast<u32>(cgltf_accessor_read_index(primitive.indices, n));
        }
      } else {
        lod.indices.resize(index_offset + vertex_count);
        for (u32 n = 0; n < vertex_count; ++n) lod.indices[index_offset + n] = vertex_offset + n;
      }

      if (tangent && tangent->count == vertex_count) {
        ReadFloats(tangent, 4, &scratch);
        for (u32 v = 0; v < vertex_count; ++v) {
          std::memcpy(lod.vertices[vertex_offset + v].tangent, &scratch[v * 4], sizeof(f32) * 4);
        }
      } else {
        GenerateTangents(&lod, vertex_offset, index_offset);
      }

      Submesh submesh;
      submesh.index_offset = index_offset;
      submesh.index_count = static_cast<u32>(lod.indices.size()) - index_offset;
      if (primitive.material) {
        submesh.material =
            out->materials[static_cast<size_t>(primitive.material - data->materials)].id;
      }
      lod.submeshes.push_back(submesh);
    }

    // Bounding sphere around the vertex centroid, for culling later.
    if (!lod.vertices.empty()) {
      f64 center[3] = {0, 0, 0};
      for (const Vertex& vertex : lod.vertices) {
        for (int axis = 0; axis < 3; ++axis) center[axis] += vertex.position[axis];
      }
      for (int axis = 0; axis < 3; ++axis) {
        mesh.bounds_center[axis] = static_cast<f32>(center[axis] / lod.vertices.size());
      }
      f32 radius_sq = 0;
      for (const Vertex& vertex : lod.vertices) {
        f32 dx = vertex.position[0] - mesh.bounds_center[0];
        f32 dy = vertex.position[1] - mesh.bounds_center[1];
        f32 dz = vertex.position[2] - mesh.bounds_center[2];
        radius_sq = std::max(radius_sq, dx * dx + dy * dy + dz * dz);
      }
      mesh.bounds_radius = std::sqrt(radius_sq);
    }
  }

  for (size_t i = 0; i < data->nodes_count; ++i) {
    const cgltf_node& node = data->nodes[i];
    if (!node.mesh) continue;
    f32 world[16];
    cgltf_node_transform_world(&node, world);

    GltfScene::Instance instance;
    instance.mesh_index = static_cast<u32>(node.mesh - data->meshes);
    instance.position = {world[12], world[13], world[14]};
    Vec3 scale{
        std::sqrt(world[0] * world[0] + world[1] * world[1] + world[2] * world[2]),
        std::sqrt(world[4] * world[4] + world[5] * world[5] + world[6] * world[6]),
        std::sqrt(world[8] * world[8] + world[9] * world[9] + world[10] * world[10])};
    instance.scale = (scale.x + scale.y + scale.z) / 3.0f;
    QuatFromMatrix(world, scale, instance.rotation);
    out->instances.push_back(instance);
  }

  REC_INFO("gltf {}: {} meshes, {} materials, {} textures, {} instances", path,
           out->meshes.size(), out->materials.size(), out->textures.size(),
           out->instances.size());
  cgltf_free(data);
  return true;
}

}  // namespace rec::asset
