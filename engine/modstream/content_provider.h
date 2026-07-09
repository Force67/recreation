#ifndef RECREATION_MODSTREAM_CONTENT_PROVIDER_H_
#define RECREATION_MODSTREAM_CONTENT_PROVIDER_H_

#include "asset/vfs.h"
#include "modstream/content_store.h"
#include "modstream/mod_resource.h"

namespace rx::modstream {

class ModCatalog;

// Mounts every resource in `manifest` into `vfs`, backed by the content store.
// Each resource becomes one FileProvider whose paths resolve through the cache,
// mounted in manifest order so a later resource overrides an earlier one exactly
// like loose-file load order. The store must outlive the vfs mounts. Every file
// the manifest names must already be present in the store (the asset-stream
// client downloads them before mounting); a missing entry is a programming error
// the provider surfaces as a failed read, not a silent gap. Used on the client.
void MountManifest(asset::Vfs& vfs, const ModManifest& manifest,
                   const ContentStore& store);

// Mounts a host's own catalogued resources into `vfs`, read straight from the
// mods directory, so a listen-server host (and a headless server's physics and
// navmesh) see exactly the content the host streams to clients. Honors
// .streamignore through the catalog, so server-only files stay unmounted. The
// catalog must outlive the vfs mounts. Used on the host.
void MountCatalog(asset::Vfs& vfs, const ModCatalog& catalog);

}  // namespace rx::modstream

#endif  // RECREATION_MODSTREAM_CONTENT_PROVIDER_H_
