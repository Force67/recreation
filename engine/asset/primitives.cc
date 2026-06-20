#include "asset/primitives.h"

#include <algorithm>

#include "core/math.h"

namespace rec::asset {
namespace {

// One bone of the test biped: name, parent index, offset from the parent joint
// in engine space. Identity bind rotation keeps local axes world-aligned so the
// locomotion's about-X swings read as forward/back.
enum Cap { kNone, kHead, kFoot, kHand };
struct BipedBone {
  const char* name;
  i32 parent;
  Vec3 offset;
  Cap cap;  // leaf cap box extending past the joint
};

// pelvis ~1m up; legs down -Y, arms out +/-X, spine up +Y.
const BipedBone kBiped[] = {
    {"NPC Root [Root]", -1, {0, 0, 0}, kNone},
    {"NPC Pelvis [Pelv]", 0, {0, 1.0f, 0}, kNone},
    {"NPC Spine [Spn0]", 1, {0, 0.18f, 0}, kNone},
    {"NPC Spine1 [Spn1]", 2, {0, 0.18f, 0}, kNone},
    {"NPC Spine2 [Spn2]", 3, {0, 0.20f, 0}, kNone},
    {"NPC Head [Head]", 4, {0, 0.22f, 0}, kHead},
    {"NPC L Thigh [LThg]", 1, {0.11f, -0.04f, 0}, kNone},
    {"NPC L Calf [LClf]", 6, {0, -0.44f, 0}, kNone},
    {"NPC L Foot [Lft ]", 7, {0, -0.44f, 0}, kFoot},
    {"NPC R Thigh [RThg]", 1, {-0.11f, -0.04f, 0}, kNone},
    {"NPC R Calf [RClf]", 9, {0, -0.44f, 0}, kNone},
    {"NPC R Foot [Rft ]", 10, {0, -0.44f, 0}, kFoot},
    {"NPC L UpperArm [LUar]", 4, {0.18f, 0.0f, 0}, kNone},
    {"NPC L Forearm [LLar]", 12, {0.28f, 0, 0}, kHand},
    {"NPC R UpperArm [RUar]", 4, {-0.18f, 0.0f, 0}, kNone},
    {"NPC R Forearm [RLar]", 14, {-0.28f, 0, 0}, kHand},
};

// Append an axis-aligned box spanning a..b inflated by `thick`, all vertices
// weighted fully to `bone`.
void AddBox(MeshLod* lod, const Vec3& a, const Vec3& b, f32 thick, u8 bone) {
  f32 lo[3] = {std::min(a.x, b.x) - thick, std::min(a.y, b.y) - thick, std::min(a.z, b.z) - thick};
  f32 hi[3] = {std::max(a.x, b.x) + thick, std::max(a.y, b.y) + thick, std::max(a.z, b.z) + thick};
  static constexpr int kFace[6][3] = {{0, 1, 2}, {0, 1, 2}, {1, 0, 2},
                                      {1, 0, 2}, {2, 0, 1}, {2, 0, 1}};
  static constexpr f32 kSign[6] = {1, -1, 1, -1, 1, -1};
  for (int f = 0; f < 6; ++f) {
    int n = kFace[f][0], u = kFace[f][1], v = kFace[f][2];
    f32 normal[3] = {0, 0, 0};
    normal[n] = kSign[f];
    u32 base = static_cast<u32>(lod->vertices.size());
    for (int c = 0; c < 4; ++c) {
      f32 cu = (c == 1 || c == 2) ? 1.0f : 0.0f;
      f32 cv = (c >= 2) ? 1.0f : 0.0f;
      Vertex vertex{};
      vertex.position[n] = kSign[f] > 0 ? hi[n] : lo[n];
      vertex.position[u] = cu > 0 ? hi[u] : lo[u];
      vertex.position[v] = cv > 0 ? hi[v] : lo[v];
      vertex.normal[n] = kSign[f];
      vertex.tangent[u] = 1.0f;
      vertex.tangent[3] = 1.0f;
      vertex.uv[0] = cu;
      vertex.uv[1] = cv;
      lod->vertices.push_back(vertex);
      SkinnedVertexExtra extra;
      extra.bone_indices[0] = bone;
      extra.bone_weights[0] = 255;
      lod->skinning.push_back(extra);
    }
    bool flip = kSign[f] < 0;
    if (flip) {
      for (u32 i : {0u, 2u, 1u, 0u, 3u, 2u}) lod->indices.push_back(base + i);
    } else {
      for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) lod->indices.push_back(base + i);
    }
  }
}

}  // namespace

Mesh MakeCube(f32 half_extent, AssetId id) {
  struct Face {
    f32 n[3];  // normal
    f32 u[3];  // tangent axes, cross(u, v) == n so corners wind ccw
    f32 v[3];
  };
  static constexpr Face kFaces[] = {
      {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},   {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
      {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},   {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
      {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
  };
  static constexpr f32 kCorners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
  static constexpr f32 kUvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

  Mesh mesh;
  mesh.id = id;
  MeshLod& lod = mesh.lods.emplace_back();
  lod.vertices.reserve(24);
  lod.indices.reserve(36);

  for (const Face& face : kFaces) {
    u32 base = static_cast<u32>(lod.vertices.size());
    for (int corner = 0; corner < 4; ++corner) {
      Vertex vertex{};
      for (int axis = 0; axis < 3; ++axis) {
        vertex.position[axis] = half_extent * (face.n[axis] + kCorners[corner][0] * face.u[axis] +
                                               kCorners[corner][1] * face.v[axis]);
        vertex.normal[axis] = face.n[axis];
        vertex.tangent[axis] = face.u[axis];
      }
      vertex.tangent[3] = 1.0f;
      vertex.uv[0] = kUvs[corner][0];
      vertex.uv[1] = kUvs[corner][1];
      lod.vertices.push_back(vertex);
    }
    for (u32 index : {0u, 1u, 2u, 0u, 2u, 3u}) lod.indices.push_back(base + index);
  }

  mesh.bounds_radius = half_extent * 1.7320508f;
  return mesh;
}

void MakeSkinnedBiped(AssetId mesh_id, Skeleton* out_skeleton, Mesh* out_mesh) {
  u32 count = static_cast<u32>(sizeof(kBiped) / sizeof(kBiped[0]));

  // Skeleton: identity bind rotation, translation from the offset table.
  out_skeleton->bones.clear();
  base::Vector<Vec3> world(count);
  for (u32 i = 0; i < count; ++i) {
    Bone bone;
    bone.name = kBiped[i].name;
    bone.parent = kBiped[i].parent;
    bone.bind_translation = kBiped[i].offset;
    bone.bind_rotation = {0, 0, 0, 1};
    bone.bind_scale = 1.0f;
    out_skeleton->bones.push_back(std::move(bone));
    world[i] = kBiped[i].parent >= 0 ? world[kBiped[i].parent] + kBiped[i].offset
                                     : kBiped[i].offset;
  }

  // Skin binding: one entry per bone (skin-local index == skeleton index), the
  // inverse bind is the inverse of the identity-rotation bind world.
  *out_mesh = Mesh{};
  out_mesh->id = mesh_id;
  out_mesh->skinned = true;
  for (u32 i = 0; i < count; ++i) {
    out_mesh->skin.bones.push_back(kBiped[i].name);
    out_mesh->skin.inverse_bind.push_back(MakeTranslation({-world[i].x, -world[i].y, -world[i].z}));
  }

  MeshLod& lod = out_mesh->lods.emplace_back();
  for (u32 i = 0; i < count; ++i) {
    // Limb box from the parent joint to this joint, weighted to the parent.
    if (kBiped[i].parent >= 0) {
      f32 thick = i >= 6 ? 0.06f : 0.09f;  // limbs thinner than the torso
      AddBox(&lod, world[kBiped[i].parent], world[i], thick,
             static_cast<u8>(kBiped[i].parent));
    }
    // Cap boxes for leaves.
    Vec3 tip = world[i];
    if (kBiped[i].cap == kHead) {
      AddBox(&lod, tip, tip + Vec3{0, 0.12f, 0}, 0.12f, static_cast<u8>(i));
    } else if (kBiped[i].cap == kFoot) {
      AddBox(&lod, tip, tip + Vec3{0, -0.02f, 0.18f}, 0.06f, static_cast<u8>(i));
    } else if (kBiped[i].cap == kHand) {
      Vec3 dir = world[i] - world[kBiped[i].parent];
      AddBox(&lod, tip, tip + Vec3{dir.x > 0 ? 0.08f : -0.08f, 0, 0}, 0.05f, static_cast<u8>(i));
    }
  }
  lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), AssetId{}});

  out_mesh->bounds_center[1] = 1.0f;
  out_mesh->bounds_radius = 2.0f;
  out_mesh->exclude_from_rt = true;  // dynamic skinned geometry stays out of the tlas
}

}  // namespace rec::asset
