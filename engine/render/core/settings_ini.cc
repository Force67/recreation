#include "render/core/settings_ini.h"

#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

namespace rec::render {
namespace {

std::string Trim(std::string_view sv) {
  size_t b = 0, e = sv.size();
  while (b < e && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
  return std::string(sv.substr(b, e - b));
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// --- enum <-> name ---------------------------------------------------------

const char* Name(AntiAliasingMode m) {
  switch (m) {
    case AntiAliasingMode::kNone: return "none";
    case AntiAliasingMode::kTaa: return "taa";
    case AntiAliasingMode::kUpscaler: return "upscaler";
  }
  return "taa";
}
bool Parse(const std::string& v, AntiAliasingMode& out) {
  if (v == "none") out = AntiAliasingMode::kNone;
  else if (v == "taa") out = AntiAliasingMode::kTaa;
  else if (v == "upscaler") out = AntiAliasingMode::kUpscaler;
  else return false;
  return true;
}

const char* Name(UpscalerKind k) {
  switch (k) {
    case UpscalerKind::kNone: return "none";
    case UpscalerKind::kFsr3: return "fsr3";
    case UpscalerKind::kDlss: return "dlss";
    case UpscalerKind::kXess: return "xess";
  }
  return "none";
}
bool Parse(const std::string& v, UpscalerKind& out) {
  if (v == "none") out = UpscalerKind::kNone;
  else if (v == "fsr3" || v == "fsr") out = UpscalerKind::kFsr3;
  else if (v == "dlss") out = UpscalerKind::kDlss;
  else if (v == "xess") out = UpscalerKind::kXess;
  else return false;
  return true;
}

const char* Name(UpscalerQuality q) {
  switch (q) {
    case UpscalerQuality::kNativeAa: return "native";
    case UpscalerQuality::kQuality: return "quality";
    case UpscalerQuality::kBalanced: return "balanced";
    case UpscalerQuality::kPerformance: return "performance";
  }
  return "quality";
}
bool Parse(const std::string& v, UpscalerQuality& out) {
  if (v == "native" || v == "nativeaa" || v == "dlaa") out = UpscalerQuality::kNativeAa;
  else if (v == "quality") out = UpscalerQuality::kQuality;
  else if (v == "balanced") out = UpscalerQuality::kBalanced;
  else if (v == "performance") out = UpscalerQuality::kPerformance;
  else return false;
  return true;
}

const char* Name(TonemapOperator t) {
  switch (t) {
    case TonemapOperator::kAces: return "aces";
    case TonemapOperator::kReinhard: return "reinhard";
    case TonemapOperator::kNone: return "none";
  }
  return "aces";
}
bool Parse(const std::string& v, TonemapOperator& out) {
  if (v == "aces") out = TonemapOperator::kAces;
  else if (v == "reinhard") out = TonemapOperator::kReinhard;
  else if (v == "none") out = TonemapOperator::kNone;
  else return false;
  return true;
}

const char* Bool(bool b) { return b ? "true" : "false"; }

}  // namespace

std::string SettingsToIni(const RenderSettings& s) {
  std::ostringstream o;
  o << "# Recreation render preset. Editable. Load/save it from the debug ui\n"
    << "# (Renderer panel -> Platform preset). Unlisted keys keep their value.\n\n";

  o << "[antialiasing]\n";
  o << "aa_mode = " << Name(s.aa_mode) << "\n";
  o << "upscaler = " << Name(s.upscaler) << "\n";
  o << "upscaler_quality = " << Name(s.upscaler_quality) << "\n";
  o << "sharpness = " << s.sharpness << "\n";
  o << "taa_history_blend = " << s.taa_history_blend << "\n\n";

  o << "[shadows]\n";
  o << "rt_shadows = " << Bool(s.rt_shadows) << "\n";
  o << "sun_angular_radius = " << s.sun_angular_radius << "\n";
  o << "shadow_maps = " << Bool(s.shadow_maps) << "\n";
  o << "shadow_resolution = " << s.shadow_resolution << "\n";
  o << "shadow_distance = " << s.shadow_distance << "\n\n";

  o << "[geometry]\n";
  o << "gpu_culling = " << Bool(s.gpu_culling) << "\n";
  o << "gpu_occlusion = " << Bool(s.gpu_occlusion) << "\n";
  o << "distance_lod = " << Bool(s.distance_lod) << "\n";
  o << "mesh_shader_lod = " << Bool(s.mesh_shader_lod) << "\n";
  o << "vsync = " << Bool(s.vsync) << "\n\n";

  o << "[sky]\n";
  o << "sky = " << Bool(s.sky) << "\n";
  o << "ibl = " << Bool(s.ibl) << "\n";
  o << "ibl_intensity = " << s.ibl_intensity << "\n";
  o << "aerial_perspective = " << s.aerial_perspective << "\n";
  o << "clouds = " << Bool(s.clouds) << "\n";
  o << "cloud_coverage = " << s.cloud_coverage << "\n\n";

  o << "[ambient_occlusion]\n";
  o << "rtao = " << Bool(s.rtao) << "\n";
  o << "ssao = " << Bool(s.ssao) << "\n";
  o << "ao_radius = " << s.ao_radius << "\n";
  o << "ao_intensity = " << s.ao_intensity << "\n";
  o << "ao_rays = " << s.ao_rays << "\n\n";

  o << "[global_illumination]\n";
  o << "ddgi = " << Bool(s.ddgi) << "\n";
  o << "ddgi_spacing = " << s.ddgi_spacing << "\n";
  o << "ddgi_intensity = " << s.ddgi_intensity << "\n";
  o << "ssgi = " << Bool(s.ssgi) << "\n\n";

  o << "[reflections]\n";
  o << "rt_reflections = " << Bool(s.rt_reflections) << "\n";
  o << "reflection_roughness_cutoff = " << s.reflection_roughness_cutoff << "\n";
  o << "water_reflections = " << Bool(s.water_reflections) << "\n";
  o << "ssr = " << Bool(s.ssr) << "\n\n";

  o << "[path_tracing]\n";
  o << "path_trace = " << Bool(s.path_trace) << "\n";
  o << "path_trace_reference = " << Bool(s.path_trace_reference) << "\n";
  o << "path_trace_spp = " << s.path_trace_spp << "\n";
  o << "path_trace_accum = " << s.path_trace_accum << "\n";
  o << "path_trace_recon = " << Bool(s.path_trace_recon) << "\n";
  o << "path_trace_recon_weight = " << s.path_trace_recon_weight << "\n";
  o << "path_trace_recon_atrous = " << s.path_trace_recon_atrous << "\n\n";

  o << "[fog]\n";
  o << "fog = " << Bool(s.fog) << "\n";
  o << "fog_density = " << s.fog_density << "\n";
  o << "fog_height_falloff = " << s.fog_height_falloff << "\n";
  o << "fog_base_height = " << s.fog_base_height << "\n";
  o << "fog_anisotropy = " << s.fog_anisotropy << "\n\n";

  o << "[post]\n";
  o << "bloom = " << Bool(s.bloom) << "\n";
  o << "bloom_intensity = " << s.bloom_intensity << "\n";
  o << "auto_exposure = " << Bool(s.auto_exposure) << "\n";
  o << "adaptation_speed = " << s.adaptation_speed << "\n";
  o << "exposure = " << s.exposure << "\n";
  o << "tonemap = " << Name(s.tonemap) << "\n";
  return o.str();
}

int ApplyIni(std::string_view text, RenderSettings& s) {
  // Collect "key = value" pairs (lowercased keys), ignoring sections/comments.
  std::map<std::string, std::string> kv;
  std::istringstream in{std::string(text)};
  std::string line;
  while (std::getline(in, line)) {
    if (auto hash = line.find_first_of(";#"); hash != std::string::npos) line.resize(hash);
    std::string t = Trim(line);
    if (t.empty() || t.front() == '[') continue;
    auto eq = t.find('=');
    if (eq == std::string::npos) continue;
    kv[Lower(Trim(std::string_view(t).substr(0, eq)))] = Trim(std::string_view(t).substr(eq + 1));
  }
  if (kv.empty()) return 0;

  int applied = 0;
  auto take = [&](const char* key, auto&& fn) {
    auto it = kv.find(key);
    if (it != kv.end() && fn(it->second)) ++applied;
  };
  auto as_bool = [](const std::string& v, bool& out) {
    const std::string l = Lower(v);
    if (l == "true" || l == "1" || l == "on" || l == "yes") { out = true; return true; }
    if (l == "false" || l == "0" || l == "off" || l == "no") { out = false; return true; }
    return false;
  };
  auto as_f32 = [](const std::string& v, f32& out) {
    try { out = std::stof(v); return true; } catch (...) { return false; }
  };
  auto as_u32 = [](const std::string& v, u32& out) {
    try { out = static_cast<u32>(std::stoul(v)); return true; } catch (...) { return false; }
  };

  auto b = [&](const char* k, bool& f) { take(k, [&](const std::string& v) { return as_bool(v, f); }); };
  auto fl = [&](const char* k, f32& f) { take(k, [&](const std::string& v) { return as_f32(v, f); }); };
  auto u = [&](const char* k, u32& f) { take(k, [&](const std::string& v) { return as_u32(v, f); }); };
  auto en = [&](const char* k, auto& f) {
    take(k, [&](const std::string& v) { return Parse(Lower(v), f); });
  };

  en("aa_mode", s.aa_mode);
  en("upscaler", s.upscaler);
  en("upscaler_quality", s.upscaler_quality);
  fl("sharpness", s.sharpness);
  fl("taa_history_blend", s.taa_history_blend);

  b("rt_shadows", s.rt_shadows);
  fl("sun_angular_radius", s.sun_angular_radius);
  b("shadow_maps", s.shadow_maps);
  u("shadow_resolution", s.shadow_resolution);
  fl("shadow_distance", s.shadow_distance);

  b("gpu_culling", s.gpu_culling);
  b("gpu_occlusion", s.gpu_occlusion);
  b("distance_lod", s.distance_lod);
  b("mesh_shader_lod", s.mesh_shader_lod);
  b("vsync", s.vsync);

  b("sky", s.sky);
  b("ibl", s.ibl);
  fl("ibl_intensity", s.ibl_intensity);
  fl("aerial_perspective", s.aerial_perspective);
  b("clouds", s.clouds);
  fl("cloud_coverage", s.cloud_coverage);

  b("rtao", s.rtao);
  b("ssao", s.ssao);
  fl("ao_radius", s.ao_radius);
  fl("ao_intensity", s.ao_intensity);
  u("ao_rays", s.ao_rays);

  b("ddgi", s.ddgi);
  fl("ddgi_spacing", s.ddgi_spacing);
  fl("ddgi_intensity", s.ddgi_intensity);
  b("ssgi", s.ssgi);

  b("rt_reflections", s.rt_reflections);
  fl("reflection_roughness_cutoff", s.reflection_roughness_cutoff);
  b("water_reflections", s.water_reflections);
  b("ssr", s.ssr);

  b("path_trace", s.path_trace);
  b("path_trace_reference", s.path_trace_reference);
  u("path_trace_spp", s.path_trace_spp);
  u("path_trace_accum", s.path_trace_accum);
  b("path_trace_recon", s.path_trace_recon);
  fl("path_trace_recon_weight", s.path_trace_recon_weight);
  u("path_trace_recon_atrous", s.path_trace_recon_atrous);

  b("fog", s.fog);
  fl("fog_density", s.fog_density);
  fl("fog_height_falloff", s.fog_height_falloff);
  fl("fog_base_height", s.fog_base_height);
  fl("fog_anisotropy", s.fog_anisotropy);

  b("bloom", s.bloom);
  fl("bloom_intensity", s.bloom_intensity);
  b("auto_exposure", s.auto_exposure);
  fl("adaptation_speed", s.adaptation_speed);
  fl("exposure", s.exposure);
  en("tonemap", s.tonemap);
  return applied;
}

bool LoadSettingsIni(const std::filesystem::path& path, RenderSettings& s) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream buf;
  buf << f.rdbuf();
  ApplyIni(buf.str(), s);
  return true;
}

bool SaveSettingsIni(const std::filesystem::path& path, const RenderSettings& s) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << SettingsToIni(s);
  return f.good();
}

}  // namespace rec::render
