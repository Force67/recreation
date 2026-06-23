#ifndef RECREATION_NET_ASSET_STREAM_H_
#define RECREATION_NET_ASSET_STREAM_H_

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <znet/z_client.h>
#include <znet/z_file_transporter.h>
#include <znet/z_server.h>

#include "core/types.h"
#include "modstream/content_store.h"
#include "modstream/mod_catalog.h"
#include "modstream/mod_resource.h"

namespace rec::net {

// Server side of FiveM-style asset streaming. Holds the catalogued server mods
// directory and feeds requested files to clients over zetanet's reliable file
// transporter. SendFile blocks (it chunks the file and waits on backpressure),
// so all streaming runs on a dedicated worker thread; the session thread only
// queues jobs and hands the manifest to joining peers.
class AssetStreamServer {
 public:
  AssetStreamServer(tx::network::ZServer& server,
                    const modstream::ModCatalog& catalog);
  ~AssetStreamServer();

  AssetStreamServer(const AssetStreamServer&) = delete;
  AssetStreamServer& operator=(const AssetStreamServer&) = delete;

  // Sends the offered manifest to a freshly joined peer, chunked over the
  // reliable data channel so an arbitrarily large catalog still fits inside the
  // single-datagram packet limit.
  void SendManifest(u32 peer);

  // Handles a client's kAssetRequest: queues each validly-catalogued content
  // hash for streaming to that peer. A hash absent from the catalog is dropped,
  // so a client can never make the server read a path outside the mods dir.
  void HandleRequest(u32 peer, const u8* data, size_t size);

 private:
  struct SendJob {
    u32 peer = 0;
    std::filesystem::path path;
  };

  void Worker();

  tx::network::ZServer& server_;
  const modstream::ModCatalog& catalog_;
  tx::network::ZFileTransporter transporter_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<SendJob> jobs_;
  bool stop_ = false;
  std::thread worker_;
};

// Client side of asset streaming. Reassembles the manifest, diffs it against the
// local content cache, requests the missing content, receives it over the file
// transporter, and reports ready once every file in the manifest is cached so
// the engine can mount it.
class AssetStreamClient {
 public:
  AssetStreamClient(tx::network::ZClient& client, modstream::ContentStore& store,
                    std::filesystem::path incoming_dir);

  // Fired once the cache holds every file the manifest names; carries the
  // manifest so the engine can mount it into the asset Vfs. Invoked on the
  // session thread from OnManifestChunk or Poll.
  void set_on_ready(std::function<void(const modstream::ModManifest&)> cb) {
    on_ready_ = std::move(cb);
  }

  // Feeds one kAssetManifest chunk. When the manifest is complete it is diffed
  // against the cache and the missing content is requested (or ready fires when
  // nothing is missing).
  void OnManifestChunk(const u8* data, size_t size);

  // Drains the control channel for incoming file chunks and advances transfers.
  // Call once per tick, after the client's Update.
  void Poll();

  bool ready() const { return ready_; }
  bool downloading() const { return downloading_; }
  bool failed() const { return failed_; }
  const modstream::ModManifest& manifest() const { return manifest_; }
  size_t files_remaining() const { return remaining_.size(); }
  u64 bytes_remaining() const;

 private:
  // One in-progress file transfer. Chunks may arrive before the file-name
  // bearing chunk 0 that bootstraps the transporter's session, so they are
  // buffered until the session exists and then streamed in index order.
  struct Transfer {
    bool started = false;
    std::map<u32, tx::network::ZFileTransporter::TransferChunk> pending;
  };

  void OnManifestComplete();
  void HandleChunk(const tx::network::ZFileTransporter::TransferChunk& chunk);
  void OnFileFinished(const std::filesystem::path& path);

  tx::network::ZClient& client_;
  modstream::ContentStore& store_;
  std::filesystem::path incoming_dir_;
  tx::network::ZFileTransporter transporter_;
  std::function<void(const modstream::ModManifest&)> on_ready_;

  std::vector<u8> manifest_buffer_;
  std::unordered_map<u32, bool> manifest_chunks_;  // received chunk indices
  u32 manifest_total_size_ = 0;
  u32 manifest_total_chunks_ = 0;
  bool manifest_started_ = false;

  modstream::ModManifest manifest_;
  std::unordered_map<modstream::ContentHash, u64> remaining_;  // hash -> size
  std::unordered_map<u64, Transfer> transfers_;                // transfer_id -> state
  bool downloading_ = false;
  bool ready_ = false;
  bool failed_ = false;
};

}  // namespace rec::net

#endif  // RECREATION_NET_ASSET_STREAM_H_
