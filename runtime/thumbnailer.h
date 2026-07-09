#ifndef RECREATION_RUNTIME_THUMBNAILER_H_
#define RECREATION_RUNTIME_THUMBNAILER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rx {

namespace asset {
struct Mesh;
}
namespace render {
class Renderer;
}

// Renders a single mesh to a small RGBA8 preview image on an off-screen target,
// for the map editor's asset cards. Self-contained: it owns a tiny offscreen
// colour+depth target, a flat-clay graphics pipeline (shaders/thumb.*), and a
// readback buffer, all driven on one-shot command buffers (no render graph).
// Compiles to a stub when ultragui/Vulkan are unavailable (the headless server
// build still links editor.cc, which holds one).
class Thumbnailer {
 public:
  Thumbnailer();
  ~Thumbnailer();
  Thumbnailer(const Thumbnailer&) = delete;
  Thumbnailer& operator=(const Thumbnailer&) = delete;

  // Borrows the renderer's Vulkan device/queue. `size` is the square preview edge
  // in pixels. Returns false (and leaves ready() false) when unavailable.
  bool Init(render::Renderer& renderer, int size = 128);
  void Shutdown();
  bool ready() const;
  int size() const;

  // Renders `mesh` (LOD 0) framed in a 3/4 orthographic view into `out` as
  // size*size RGBA8 pixels (transparent background). False on any failure.
  bool Render(const asset::Mesh& mesh, std::vector<std::uint8_t>& out);

  // Disk cache helpers (PNG at the configured size). LoadCached returns false if
  // the file is missing or not size*size.
  bool LoadCached(const std::string& path, std::vector<std::uint8_t>& out) const;
  void SaveCached(const std::string& path, const std::vector<std::uint8_t>& rgba) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rx

#endif  // RECREATION_RUNTIME_THUMBNAILER_H_
