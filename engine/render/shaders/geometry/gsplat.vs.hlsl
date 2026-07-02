#include "rhi_bindings.hlsli"
// 3D Gaussian splatting (Kerbl et al. 2023): a non-triangle primitive path.
// Each gaussian is vertex-pulled into a screen-space ellipse by projecting its
// 3D covariance to 2D (EWA splatting). The pixel shader evaluates the gaussian
// through the conic. Sorted back-to-front on the cpu, alpha blended.
struct Gaussian {
  float3 position;
  float opacity;
  float3 scale;
  float pad0;
  float4 rotation;  // quaternion xyzw
  float3 color;
  float pad1;
};
[[vk::binding(0, 0)]] StructuredBuffer<Gaussian> gaussians : register(t0, space0);

struct PushData {
  column_major float4x4 view;  // world -> view
  float proj_x;                // P[0][0]
  float proj_y;                // P[1][1] (negative, vulkan y-flip)
  float near_plane;
  float screen_x;
  float screen_y;
  float pad0;
  float pad1;
  float pad2;
};
PUSH_CONSTANTS(PushData, push);

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 offset : TEXCOORD0;  // pixel offset from the splat center
  [[vk::location(1)]] float3 conic : TEXCOORD1;   // inverse 2d covariance (a, b, c)
  [[vk::location(2)]] float4 color : COLOR0;       // rgb, opacity
};

static const float2 kCorners[4] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1)};

float3x3 QuatToMat(float4 q) {
  float x = q.x, y = q.y, z = q.z, w = q.w;
  return float3x3(1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                  2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                  2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y));
}

VsOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  Gaussian g = gaussians[iid];
  VsOut o;

  float3 t = mul(push.view, float4(g.position, 1.0)).xyz;  // view space (front = -z)
  if (t.z >= -push.near_plane) {  // behind / too close: collapse off-screen
    o.pos = float4(2, 2, 2, 1);
    o.offset = 0;
    o.conic = float3(1, 0, 1);
    o.color = 0;
    return o;
  }
  float z = -t.z;

  // 3d covariance Sigma = R S S^T R^T.
  float3x3 R = QuatToMat(g.rotation);
  float3x3 S = float3x3(g.scale.x, 0, 0, 0, g.scale.y, 0, 0, 0, g.scale.z);
  float3x3 M = mul(R, S);
  float3x3 sigma = mul(M, transpose(M));

  // Project to 2d: T = J * W, cov2d = T Sigma T^T (top-left 2x2). Focal in px.
  float fx = push.proj_x * push.screen_x * 0.5;
  float fy = push.proj_y * push.screen_y * 0.5;
  float2x3 J = float2x3(fx / z, 0, fx * t.x / (z * z), 0, fy / z, fy * t.y / (z * z));
  float3x3 W = (float3x3)push.view;
  float2x3 Tm = mul(J, W);
  float2x2 cov = mul(mul(Tm, sigma), transpose(Tm));
  cov[0][0] += 0.3;  // low-pass: minimum one-pixel splat
  cov[1][1] += 0.3;

  float det = cov[0][0] * cov[1][1] - cov[0][1] * cov[1][0];
  if (det <= 0.0) {
    o.pos = float4(2, 2, 2, 1);
    o.offset = 0;
    o.conic = float3(1, 0, 1);
    o.color = 0;
    return o;
  }
  float inv_det = 1.0 / det;
  o.conic = float3(cov[1][1] * inv_det, -cov[0][1] * inv_det, cov[0][0] * inv_det);

  // Screen extent: 3 sigma of the larger eigenvalue, in pixels.
  float mid = 0.5 * (cov[0][0] + cov[1][1]);
  float lambda = mid + sqrt(max(0.1, mid * mid - det));
  float radius = ceil(3.0 * sqrt(lambda));

  float4 clip = float4(push.proj_x * t.x, push.proj_y * t.y, push.near_plane, z);
  float2 corner = kCorners[vid];
  o.offset = corner * radius;
  // Offset the clip-space center by the pixel extent (converted to ndc * w).
  clip.x += corner.x * radius * (2.0 / push.screen_x) * clip.w;
  clip.y += corner.y * radius * (2.0 / push.screen_y) * clip.w;
  o.pos = clip;
  o.color = float4(g.color, g.opacity);
  return o;
}
