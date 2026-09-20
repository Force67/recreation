#include "components/gamenet/asset_stream.h"
#include "core/log.h"

#include <base/algorithm.h>
#include <base/filesystem/path.h>

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "components/gamenet/protocol.h"
#include "components/modstream/asset_request.h"
#include "components/modstream/client_scripts.h"
#include "components/modstream/manifest_chunk.h"
#include "components/modstream/manifest_codec.h"
#include "components/modstream/transfer_plan.h"
#include "net/znet_util.h"

namespace rx::net {
namespace {

namespace fs = std::filesystem;

// Content hashes per kAssetRequest packet, kept under the datagram ceiling
// (8 bytes each plus a 4-byte count). Larger plans split across packets.
constexpr u32 kRequestHashesPerPacket = 6000;

// Client-script entries accepted per kClientScripts packet. A real server stays
// far under this; anything larger is treated as malformed.
constexpr size_t kMaxClientScriptEntries = 16384;

base::Path ToBasePath(const fs::path& path) {
  return base::Path(path.string().c_str());
}

}  // namespace

// --- server ---

AssetStreamServer::AssetStreamServer(tx::network::ZServer& server,
                                     const modstream::ModCatalog& catalog,
                                     unsigned sender_threads)
    : server_(server), catalog_(&catalog), transporter_(server) {
  const unsigned count = sender_threads > 0 ? sender_threads : 1;
  workers_.reserve(count);
  for (unsigned i = 0; i < count; ++i)
    workers_.emplace_back(&AssetStreamServer::Worker, this);
}

AssetStreamServer::~AssetStreamServer() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  // Wake idle workers so they observe stop_ and exit. A worker mid-SendFile
  // finishes (or hits the per-transfer backpressure timeout) before joining;
  // zetanet's transporter has no send-abort, so shutdown waits that out.
  cv_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable())
      worker.join();
  }
}

void AssetStreamServer::SendManifest(u32 peer) {
  const std::vector<u8> bytes = modstream::EncodeManifest(catalog_->manifest());
  const u32 total = static_cast<u32>(bytes.size());
  const u32 chunks = modstream::ManifestChunkCount(total);
  for (u32 i = 0; i < chunks; ++i) {
    const u32 offset = i * modstream::kManifestChunkPayload;
    const u32 len = std::min<u32>(modstream::kManifestChunkPayload, total - offset);
    server_.Push(MakePacket(peer, static_cast<u16>(GameMessage::kAssetManifest),
                            modstream::EncodeManifestChunk(manifest_generation_, total, chunks, i,
                                                           bytes.data() + offset, len),
                            /*reliable=*/true, tx::network::PacketPriority::High));
  }
  RX_INFO("net: sent manifest ({} files, {} bytes) to peer {}", catalog_->manifest().TotalFiles(),
          catalog_->manifest().TotalBytes(), peer);
}

void AssetStreamServer::SendClientScripts(u32 peer) {
  std::vector<modstream::ClientScriptEntry> entries;
  for (const modstream::ModResource& resource : catalog_->manifest().resources) {
    for (const base::String& script : resource.client_scripts) {
      const auto* file =
          base::FindIf(resource.files.begin(), resource.files.end(),
                       [&script](const modstream::ResourceFile& f) { return f.path == script; });
      if (file == resource.files.end())
        continue;  // the catalog only records validated scripts; never trip here
      entries.push_back({file->hash, file->size,
                         std::string(resource.name.c_str(), resource.name.size()) + "/" +
                             std::string(script.c_str(), script.size())});
    }
  }
  server_.Push(MakePacket(peer, static_cast<u16>(GameMessage::kClientScripts),
                          modstream::EncodeClientScripts(manifest_generation_, entries),
                          /*reliable=*/true, tx::network::PacketPriority::High));
  if (!entries.empty())
    RX_INFO("net: offered {} client script(s) to peer {}", entries.size(), peer);
}

void AssetStreamServer::HandleRequest(u32 peer, const u8* data, size_t size) {
  std::optional<std::vector<modstream::ContentHash>> hashes =
      modstream::DecodeHashRequest(data, size, kRequestHashesPerPacket);
  if (!hashes)
    return;  // malformed or oversized request, drop

  size_t queued = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (modstream::ContentHash hash : *hashes) {
      base::Optional<fs::path> path = catalog_->PathForHash(hash);
      if (!path)
        continue;  // not catalogued: never read outside the mods dir
      jobs_.push_back({peer, std::move(*path)});
      ++queued;
    }
  }
  if (queued > 0)
    cv_.notify_all();
}

void AssetStreamServer::Worker() {
  for (;;) {
    SendJob job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
      if (stop_ && jobs_.empty())
        return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    if (!transporter_.SendFile(ToBasePath(job.path), tx::network::ZPeerId(job.peer))) {
      RX_WARN("net: asset stream failed to send {} to peer {}", job.path.string(), job.peer);
    }
  }
}

// --- client ---

AssetStreamClient::AssetStreamClient(tx::network::ZClient& client,
                                     modstream::ContentStore& store,
                                     fs::path incoming_dir)
    : client_(client), store_(store), incoming_dir_(std::move(incoming_dir)), transporter_(client) {
  std::error_code ec;
  fs::create_directories(incoming_dir_.c_str(), ec);
  // File-transfer chunks arrive as system packets on zetanet's control channel.
  // The client session drains that channel each tick and routes them to
  // OnFilePacket (see ClientSession::Tick), since ZClient::Update would
  // otherwise drain and discard them.
}

AssetStreamClient::~AssetStreamClient() = default;

void AssetStreamClient::OnFilePacket(const tx::network::IncomingPacket& packet) {
  tx::network::ZFileTransporter::TransferChunk chunk;
  if (transporter_.ParseTransferChunkPacket(packet, chunk))
    HandleChunk(chunk);
}

u64 AssetStreamClient::bytes_remaining() const {
  u64 total = 0;
  for (const auto& [hash, size] : remaining_)
    total += size;
  return total;
}

void AssetStreamClient::OnManifestChunk(const u8* data, size_t size) {
  // The codec validates the chunk in isolation (header, counts, index, payload
  // length), so reassembly below cannot write out of range.
  std::optional<modstream::ManifestChunkView> chunk = modstream::DecodeManifestChunk(data, size);
  if (!chunk)
    return;

  // Process a chunk only for a newer generation (a fresh manifest, from join or a
  // live reload) or for the generation currently being assembled. A chunk for an
  // already-finished generation is a stale retransmit and is ignored.
  const bool newer = chunk->generation > manifest_generation_;
  const bool same_assembly = manifest_started_ && chunk->generation == manifest_generation_;
  if (!newer && !same_assembly)
    return;
  if (newer) {
    ResetForNewManifest(chunk->generation);
    manifest_generation_ = chunk->generation;
  }

  if (!manifest_started_) {
    manifest_total_size_ = chunk->total_size;
    manifest_total_chunks_ = chunk->total_chunks;
    manifest_buffer_.assign(chunk->total_size, 0);
    manifest_started_ = true;
  } else if (chunk->total_size != manifest_total_size_ ||
             chunk->total_chunks != manifest_total_chunks_) {
    return;  // a chunk that disagrees with the first one is corrupt
  }

  const u32 offset = chunk->chunk_index * modstream::kManifestChunkPayload;
  std::copy(chunk->payload, chunk->payload + chunk->payload_len, manifest_buffer_.begin() + offset);
  manifest_chunks_[chunk->chunk_index] = true;
  if (manifest_chunks_.size() == manifest_total_chunks_)
    OnManifestComplete();
}

void AssetStreamClient::ResetForNewManifest(u32 incoming_generation) {
  manifest_buffer_.clear();
  manifest_chunks_.clear();
  manifest_total_size_ = 0;
  manifest_total_chunks_ = 0;
  manifest_started_ = false;
  remaining_.clear();
  transfers_.clear();
  planned_files_ = 0;
  downloading_ = false;
  ready_ = false;
  failed_ = false;
  manifest_hashes_.clear();
  // A reload re-sends the script offer with the fresh manifest. Already-loaded
  // assemblies stay loaded (the engine ALC is not collectible); a changed
  // assembly applies on the next join. An offer that already arrived for the
  // incoming (or a newer) generation is kept: the reliable channel may deliver
  // it ahead of the manifest it belongs with.
  if (offered_generation_ < incoming_generation) {
    offered_scripts_.clear();
    offered_generation_ = 0;
  }
  client_scripts_.clear();
  scripts_delivered_ = false;
}

void AssetStreamClient::OnClientScripts(const u8* data, size_t size) {
  std::optional<modstream::ClientScriptOffer> decoded =
      modstream::DecodeClientScripts(data, size, kMaxClientScriptEntries);
  if (!decoded) {
    RX_WARN("net: dropped a corrupt client-script offer");
    return;
  }
  // Tagged with the manifest generation like the manifest chunks are: an offer
  // for a generation older than the one being assembled (a delayed duplicate
  // from before a live reload) is dropped rather than allowed to overwrite the
  // current one. A newer offer is stored and waits for its manifest; only one
  // for the current assembly is validated immediately.
  if (decoded->generation < manifest_generation_)
    return;
  offered_scripts_ = std::move(decoded->entries);
  offered_generation_ = decoded->generation;
  if (decoded->generation == manifest_generation_ && !manifest_hashes_.empty())
    ValidateScripts();
}

void AssetStreamClient::ValidateScripts() {
  client_scripts_.clear();
  for (const modstream::ClientScriptEntry& entry : offered_scripts_) {
    if (manifest_hashes_.find(entry.hash) == manifest_hashes_.end()) {
      RX_WARN("net: server offered client script '{}' whose bytes are not in the manifest; "
              "ignoring it",
              entry.path.c_str());
      continue;
    }
    client_scripts_.push_back(entry);
  }
  if (!client_scripts_.empty())
    RX_INFO("net: server offers {} client script(s)", client_scripts_.size());
  DeliverScriptsIfReady();
}

void AssetStreamClient::DeliverScriptsIfReady() {
  if (scripts_delivered_ || !ready_ || !on_scripts_)
    return;
  // The offer for THIS generation must have arrived: the server always sends
  // one per generation (possibly empty), so a mismatch means it is still in
  // flight and delivery waits for it.
  if (offered_generation_ != manifest_generation_)
    return;
  scripts_delivered_ = true;
  on_scripts_(client_scripts_);
}

void AssetStreamClient::OnManifestComplete() {
  std::optional<modstream::ModManifest> decoded = modstream::DecodeManifest(manifest_buffer_);
  if (!decoded) {
    RX_ERROR("net: received a corrupt asset manifest");
    failed_ = true;
    return;
  }
  manifest_ = std::move(*decoded);

  // Index the manifest's hashes so the script offer can be validated against
  // exactly the files this server streams.
  manifest_hashes_.clear();
  for (const modstream::ModResource& resource : manifest_.resources) {
    for (const modstream::ResourceFile& file : resource.files)
      manifest_hashes_.insert(file.hash);
  }
  if (offered_generation_ == manifest_generation_ && !offered_scripts_.empty())
    ValidateScripts();

  const std::vector<modstream::NeededFile> plan = modstream::ComputeMissing(manifest_, store_);
  if (plan.empty()) {
    RX_INFO("net: asset manifest complete, {} files already cached", manifest_.TotalFiles());
    ready_ = true;
    SendReady();
    if (on_ready_)
      on_ready_(manifest_);
    DeliverScriptsIfReady();
    return;
  }

  remaining_.reserve(plan.size());
  for (const modstream::NeededFile& need : plan)
    remaining_[need.hash] = need.size;
  planned_files_ = plan.size();
  downloading_ = true;
  RX_INFO("net: streaming {} mod files ({} bytes) from the server", plan.size(),
          modstream::PlannedBytes(plan));

  // Request the missing hashes, split across packets to stay under the datagram
  // ceiling. Each packet is independent; the server queues every valid hash.
  for (size_t base = 0; base < plan.size(); base += kRequestHashesPerPacket) {
    const size_t end = std::min(base + kRequestHashesPerPacket, plan.size());
    std::vector<modstream::ContentHash> batch;
    batch.reserve(end - base);
    for (size_t i = base; i < end; ++i)
      batch.push_back(plan[i].hash);
    client_.Push(MakePacket(
        tx::network::ZPeerId::to_server, static_cast<u16>(GameMessage::kAssetRequest),
        modstream::EncodeHashRequest(batch), /*reliable=*/true, tx::network::PacketPriority::High));
  }
}

void AssetStreamClient::HandleChunk(const tx::network::ZFileTransporter::TransferChunk& chunk) {
  const u64 transfer_id = chunk.transfer_id;
  Transfer& transfer = transfers_[transfer_id];
  transfer.pending[chunk.chunk_index] = chunk;

  // The transporter's session is created by the file-name bearing chunk 0; until
  // it arrives, later chunks (already ACKed, so never resent) wait buffered.
  if (!transfer.started && transfer.pending.find(0) == transfer.pending.end())
    return;
  transfer.started = true;

  const base::Path temp_dir = ToBasePath(incoming_dir_);
  for (auto it = transfer.pending.begin(); it != transfer.pending.end();) {
    bool completed = false;
    if (!transporter_.StreamChunkToFile(it->second, temp_dir, &completed)) {
      RX_WARN("net: asset stream write failed for transfer {}", transfer_id);
      failed_ = true;
      transporter_.AbortStreamedFile(transfer_id);  // drop the half-written temp file
      transfers_.erase(transfer_id);
      return;
    }
    it = transfer.pending.erase(it);
    if (!completed)
      continue;

    const fs::path out = incoming_dir_ / (std::to_string(transfer_id) + ".bin");
    if (transporter_.FinalizeStreamedFile(transfer_id, ToBasePath(out))) {
      OnFileFinished(out);
    } else {
      RX_WARN("net: asset stream finalize failed for transfer {}", transfer_id);
      failed_ = true;
      transporter_.AbortStreamedFile(transfer_id);  // finalize already failed; clean up
    }
    transfers_.erase(transfer_id);
    return;
  }
}

void AssetStreamClient::OnFileFinished(const fs::path& path) {
  base::Optional<modstream::ContentHash> hash = store_.Ingest(path);
  if (!hash) {
    RX_WARN("net: failed to cache a streamed mod file");
    failed_ = true;
    return;
  }
  remaining_.erase(*hash);

  // Progress, throttled so a many-file download does not flood the log: every
  // file for a small set, otherwise at roughly ten-percent steps.
  if (downloading_ && !remaining_.empty() && planned_files_ > 0) {
    const size_t done = planned_files_ - remaining_.size();
    const size_t step = planned_files_ <= 20 ? 1 : planned_files_ / 10;
    if (done % step == 0) {
      RX_INFO("net: streamed {}/{} mod files ({} bytes left)", done, planned_files_,
              bytes_remaining());
    }
  }

  if (downloading_ && remaining_.empty()) {
    downloading_ = false;
    ready_ = true;
    RX_INFO("net: all mod assets streamed, mounting {} files", manifest_.TotalFiles());
    SendReady();
    if (on_ready_)
      on_ready_(manifest_);
    DeliverScriptsIfReady();
  }
}

void AssetStreamClient::SendReady() {
  client_.Push(MakePacket(tx::network::ZPeerId::to_server,
                          static_cast<u16>(GameMessage::kAssetReady), std::vector<u8>{},
                          /*reliable=*/true, tx::network::PacketPriority::High));
}

}  // namespace rx::net
