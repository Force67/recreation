// Compile-time verification (and future use site) of kinema's header-only
// Jolt adapter: skeleton conversion plus hard/soft ragdoll pose keying. The
// adapter needs a TU that sees both Jolt and kinema headers; this module is
// the only place with that view.

#include <kinema/jolt_adapter.h>

namespace rec::physics {

// Anchors the adapter's inline definitions so the linker proves them out.
JPH::Ref<JPH::Skeleton> MakeJoltSkeleton(const kinema::Skeleton& skeleton) {
  return kinema::jolt::MakeSkeleton(skeleton);
}

}  // namespace rec::physics
