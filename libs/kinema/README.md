# kinema

A small, data-oriented skeletal animation runtime. Built for the *recreation*
engine but deliberately self-contained: no engine types, no exceptions, no
RTTI, no dependencies beyond the C++ standard library. Pairs naturally with
[Jolt Physics](https://github.com/jrouwe/JoltPhysics) through a header-only
adapter.

## Why it's fast

- **Transcode once, stream forever.** Clips are baked into a relocatable
  binary blob: uniformly sampled keys, 16-bit range-quantized, stored
  frame-major SoA. Sampling a pose is two contiguous row reads and a lerp —
  no curve evaluation, no branches per bone, no allocation. Constant tracks
  are detected at build time and stored once at full precision.
- **Poses are SoA views** over arena memory; blend operations are flat
  kernels over whole arrays that autovectorize.
- **Blend trees run as compiled programs** (a flat `PoseOp` list over pose
  registers), so authoring structure never appears in the hot path.
- **Transitions are inertialized** — capture the pose offset at the switch
  and decay it C2-smoothly — so a transition evaluates one graph, not two.

## Use

```cmake
add_subdirectory(libs/kinema)
target_link_libraries(game PRIVATE kinema::kinema)
```

```cpp
#include <kinema/kinema.h>

// Import: feed uniformly sampled poses from your source format.
kinema::ClipBuilder builder(num_bones, num_frames, 30.0f);
for (frame, bone : source) builder.SetSample(frame, bone, t, r, s);
builder.AddEvent("FootLeft", 0.43f);
builder.AddRootKey(duration, total_displacement);
kinema::OwnedClip clip(builder.Build());   // blob is disk-cacheable as-is

// Runtime: registers + a compiled program per actor archetype.
kinema::PoseArena arena(num_bones, 4);
kinema::PoseOp program[] = {
    {.kind = kinema::PoseOp::Kind::kSample, .dst = 0, .clip = clip.get(), .time = t},
    {.kind = kinema::PoseOp::Kind::kSample, .dst = 1, .clip = other, .time = t2},
    {.kind = kinema::PoseOp::Kind::kBlend, .dst = 2, .a = 0, .b = 1, .alpha = 0.3f},
};
kinema::PoseView pose = kinema::ExecuteProgram(program, 3, arena);
```

Jolt integration (`kinema/jolt_adapter.h`, header-only, include where Jolt
headers are visible): `MakeSkeleton`, `SetRagdollPose` (hard keying),
`DriveRagdollPose` (motor-driven soft keying for physical hit reactions).

## Testing

`-DKINEMA_BUILD_TESTS=ON` builds `kinematest`, a synthetic round-trip suite
(no game data needed). In recreation, `hkxinfo --kinema` cross-validates the
transcoder against the reference Havok spline sampler over real game clips
and reports compression + performance numbers.
