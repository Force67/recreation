#include "net/asset_stream.h"

#include <algorithm>
#include <utility>

#include <base/filesystem/path.h>

#include "core/log.h"
#include "modstream/manifest_codec.h"
#include "modstream/transfer_plan.h"
#include "net/protocol.h"

namespace rec::net {
namespace {

namespace fs = std::filesystem;

// Manifest bytes carried per kAssetManifest packet. A datagram cannot be
// fragmented, so this stays well under the ~64 KiB UDP payload ceiling with room
// for the chunk header and zetanet's own framing.
constexpr u32 kManifestChunkPayload = 60000;
// Refuse a reassembled manifest larger than this; a real catalog is far smaller,
// and the cap bounds the client's reassembly buffer against a hostile header.
constexpr u32 kMaxManifestSize = 64u * 1024 * 1024;
// Content hashes per kAssetRequest packet, kept under the datagram ceiling
// (8 bytes each plus a 4-byte count). Larger plans split across packets.
constexpr u32 kRequestHashesPerPacket = 6000;

base::Path ToBasePath(const fs::path& path) {
  return base::Path(path.string().c_str());
}

void PutU32(std::vector<u8>& out, u32 v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

void PutU64(std::vector<u8>& out, u64 v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

u32 LoadU32(const u8* p) {
  return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 |
         static_cast<u32>(p[2]) << 16 | static_cast<u32>(p[3]) << 24;
}

u64 LoadU64(const u8* p) {
  u64 v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<u64>(p[i]) << (8 * i);
  return v;
}

u32 ManifestChunkCount(u32 total_size) {
  return (total_size + kManifestChunkPayload - 1) / kManifestChunkPayload;
}

}  // namespace

// --- server ---

AssetStreamServer::AssetStreamServer(tx::network::ZServer& server,
                                     const modstream::ModCatalog& catalog)
    : server_(server), catalog_(catalog), transporter_(server) {
  worker_ = std::thread(&AssetStreamServer::Worker, this);
}

AssetStreamServer::~AssetStreamServer() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void AssetStreamServer::SendManifest(u32 peer) {
  const std::vector<u8> bytes = modstream::EncodeManifest(catalog_.manifest());
  const u32 total = static_cast<u32>(bytes.size());
  const u32 chunks = ManifestChunkCount(total);
  for (u32 i = 0; i < chunks; ++i) {
    const u32 offset = i * kManifestChunkPayload;
    const u32 len = std::min<u32>(kManifestChunkPayload, total - offset);
    std::vector<u8> payload;
    payload.reserve(12 + len);
    PutU32(payload, total);
    PutU32(payload, chunks);
    PutU32(payload, i);
    payload.insert(payload.end(), bytes.begin() + offset, bytes.begin() + offset + len);
    server_.Push(MakePacket(peer, MessageType::kAssetManifest, payload,
                            /*reliable=*/true, tx::network::PacketPriority::High));
  }
  REC_INFO("net: sent manifest ({} files, {} bytes) to peer {}",
           catalog_.manifest().TotalFiles(), catalog_.manifest().TotalBytes(), peer);
}

void AssetStreamServer::HandleRequest(u32 peer, const u8* data, size_t size) {
  if (size < 4) return;
  const u32 count = LoadU32(data);
  if (count > kRequestHashesPerPacket) return;  // oversized, drop
  if (size != static_cast<size_t>(4) + static_cast<size_t>(count) * 8) return;

  size_t queued = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (u32 i = 0; i < count; ++i) {
      const modstream::ContentHash hash = LoadU64(data + 4 + static_cast<size_t>(i) * 8);
      std::optional<fs::path> path = catalog_.PathForHash(hash);
      if (!path) continue;  // not catalogued: never read outside the mods dir
      jobs_.push_back({peer, std::move(*path)});
      ++queued;
    }
  }
  if (queued > 0) cv_.notify_all();
}

void AssetStreamServer::Worker() {
  for (;;) {
    SendJob job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
      if (stop_ && jobs_.empty()) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    if (!transporter_.SendFile(ToBasePath(job.path), tx::network::ZPeerId(job.peer))) {
      REC_WARN("net: asset stream failed to send {} to peer {}",
               job.path.string(), job.peer);
    }
  }
}

// --- client ---

AssetStreamClient::AssetStreamClient(tx::network::ZClient& client,
                                     modstream::ContentStore& store,
                                     fs::path incoming_dir)
    : client_(client),
      store_(store),
      incoming_dir_(std::move(incoming_dir)),
      transporter_(client) {
  std::error_code ec;
  fs::create_directories(incoming_dir_, ec);
  // ZClient::Update drains the control channel; without this sink the incoming
  // file chunks would be discarded there. The sink runs on the session thread.
  client_.SetFileTransferSink(&AssetStreamClient::SinkThunk, this);
}

AssetStreamClient::~AssetStreamClient() {
  client_.SetFileTransferSink(nullptr, nullptr);
}

void AssetStreamClient::SinkThunk(void* context,
                                  const tx::network::IncomingPacket& packet) {
  auto* self = static_cast<AssetStreamClient*>(context);
  tx::network::ZFileTransporter::TransferChunk chunk;
  if (self->transporter_.ParseTransferChunkPacket(packet, chunk)) self->HandleChunk(chunk);
}

u64 AssetStreamClient::bytes_remaining() const {
  u64 total = 0;
  for (const auto& [hash, size] : remaining_) total += size;
  return total;
}

void AssetStreamClient::OnManifestChunk(const u8* data, size_t size) {
  if (downloading_ || ready_ || failed_) return;  // manifest already resolved
  if (size < 12) return;

  const u32 total = LoadU32(data);
  const u32 chunks = LoadU32(data + 4);
  const u32 index = LoadU32(data + 8);
  const size_t payload_len = size - 12;

  if (total == 0 || total > kMaxManifestSize) return;
  if (chunks != ManifestChunkCount(total) || index >= chunks) return;

  const u32 offset = index * kManifestChunkPayload;
  const u32 expected_len = std::min<u32>(kManifestChunkPayload, total - offset);
  if (payload_len != expected_len) return;

  if (!manifest_started_) {
    manifest_total_size_ = total;
    manifest_total_chunks_ = chunks;
    manifest_buffer_.assign(total, 0);
    manifest_started_ = true;
  } else if (total != manifest_total_size_ || chunks != manifest_total_chunks_) {
    return;  // a chunk that disagrees with the first one is corrupt
  }

  std::copy(data + 12, data + size, manifest_buffer_.begin() + offset);
  manifest_chunks_[index] = true;
  if (manifest_chunks_.size() == manifest_total_chunks_) OnManifestComplete();
}

void AssetStreamClient::OnManifestComplete() {
  std::optional<modstream::ModManifest> decoded =
      modstream::DecodeManifest(manifest_buffer_);
  if (!decoded) {
    REC_ERROR("net: received a corrupt asset manifest");
    failed_ = true;
    return;
  }
  manifest_ = std::move(*decoded);

  const std::vector<modstream::NeededFile> plan =
      modstream::ComputeMissing(manifest_, store_);
  if (plan.empty()) {
    REC_INFO("net: asset manifest complete, {} files already cached",
             manifest_.TotalFiles());
    ready_ = true;
    if (on_ready_) on_ready_(manifest_);
    return;
  }

  remaining_.reserve(plan.size());
  for (const modstream::NeededFile& need : plan) remaining_[need.hash] = need.size;
  downloading_ = true;
  REC_INFO("net: streaming {} mod files ({} bytes) from the server", plan.size(),
           modstream::PlannedBytes(plan));

  // Request the missing hashes, split across packets to stay under the datagram
  // ceiling. Each packet is independent; the server queues every valid hash.
  for (size_t base = 0; base < plan.size(); base += kRequestHashesPerPacket) {
    const size_t end = std::min(base + kRequestHashesPerPacket, plan.size());
    std::vector<u8> payload;
    payload.reserve(4 + (end - base) * 8);
    PutU32(payload, static_cast<u32>(end - base));
    for (size_t i = base; i < end; ++i) PutU64(payload, plan[i].hash);
    client_.Push(MakePacket(tx::network::ZPeerId::to_server, MessageType::kAssetRequest,
                            payload, /*reliable=*/true, tx::network::PacketPriority::High));
  }
}

void AssetStreamClient::HandleChunk(
    const tx::network::ZFileTransporter::TransferChunk& chunk) {
  const u64 transfer_id = chunk.transfer_id;
  Transfer& transfer = transfers_[transfer_id];
  transfer.pending[chunk.chunk_index] = chunk;

  // The transporter's session is created by the file-name bearing chunk 0; until
  // it arrives, later chunks (already ACKed, so never resent) wait buffered.
  if (!transfer.started && transfer.pending.find(0) == transfer.pending.end()) return;
  transfer.started = true;

  const base::Path temp_dir = ToBasePath(incoming_dir_);
  for (auto it = transfer.pending.begin(); it != transfer.pending.end();) {
    bool completed = false;
    if (!transporter_.StreamChunkToFile(it->second, temp_dir, &completed)) {
      REC_WARN("net: asset stream write failed for transfer {}", transfer_id);
      failed_ = true;
      transporter_.AbortStreamedFile(transfer_id);  // drop the half-written temp file
      transfers_.erase(transfer_id);
      return;
    }
    it = transfer.pending.erase(it);
    if (!completed) continue;

    const fs::path out = incoming_dir_ / (std::to_string(transfer_id) + ".bin");
    if (transporter_.FinalizeStreamedFile(transfer_id, ToBasePath(out))) {
      OnFileFinished(out);
    } else {
      REC_WARN("net: asset stream finalize failed for transfer {}", transfer_id);
      failed_ = true;
      transporter_.AbortStreamedFile(transfer_id);  // finalize already failed; clean up
    }
    transfers_.erase(transfer_id);
    return;
  }
}

void AssetStreamClient::OnFileFinished(const fs::path& path) {
  std::optional<modstream::ContentHash> hash = store_.Ingest(path);
  if (!hash) {
    REC_WARN("net: failed to cache a streamed mod file");
    failed_ = true;
    return;
  }
  remaining_.erase(*hash);
  if (downloading_ && remaining_.empty()) {
    downloading_ = false;
    ready_ = true;
    REC_INFO("net: all mod assets streamed, mounting {} files", manifest_.TotalFiles());
    if (on_ready_) on_ready_(manifest_);
  }
}

}  // namespace rec::net
