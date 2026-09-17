#ifndef RECREATION_MASTERLIST_LOAD_ORDER_DIGEST_H_
#define RECREATION_MASTERLIST_LOAD_ORDER_DIGEST_H_

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "core/types.h"

namespace rx::masterlist {

// A short digest of a load order, computed the same way on both sides so a
// client can tell before it dials whether the host's world is one it can load.
// The listing shows the digest and the count; a mismatch is what turns
// "failed to load" into "your load order is not this server's".
//
// Order matters (a different order is a different world) and case does not
// (Windows and Linux disagree about it, the same install must digest alike).
base::String LoadOrderDigest(const base::Vector<base::String>& plugins);

}  // namespace rx::masterlist

#endif  // RECREATION_MASTERLIST_LOAD_ORDER_DIGEST_H_
