#include "asset/materialx.h"

#include <fstream>
#include <sstream>

#include "core/log.h"

namespace rec::asset {
namespace {

// Value of an attribute key="..." inside a single tag's text.
std::string Attr(const std::string& tag, const char* key) {
  std::string pat = std::string(key) + "=\"";
  size_t p = tag.find(pat);
  if (p == std::string::npos) return "";
  p += pat.size();
  size_t e = tag.find('"', p);
  if (e == std::string::npos) return "";
  return tag.substr(p, e - p);
}

// MaterialX vector/color values are comma separated; parse up to n floats.
void ParseFloats(const std::string& value, f32* out, int n) {
  std::string s = value;
  for (char& c : s) {
    if (c == ',') c = ' ';
  }
  std::istringstream ss(s);
  f32 v;
  for (int i = 0; i < n && (ss >> v); ++i) out[i] = v;
}

}  // namespace

bool LoadMaterialX(const std::string& path, Material* out) {
  std::ifstream file(path);
  if (!file) {
    REC_WARN("materialx: cannot open {}", path);
    return false;
  }
  std::stringstream buf;
  buf << file.rdbuf();
  std::string doc = buf.str();
  if (doc.find("standard_surface") == std::string::npos) {
    REC_WARN("materialx: {} has no standard_surface node", path);
    return false;
  }

  // standard_surface inputs that are not 1:1 engine fields get combined below.
  f32 base_weight = 1.0f;
  f32 sheen_weight = 0.0f, sheen_color[3] = {1, 1, 1};
  f32 emission_weight = 0.0f, emission_color[3] = {1, 1, 1};
  f32 thin_film_thickness = 0.0f;

  size_t p = 0;
  while ((p = doc.find("<input", p)) != std::string::npos) {
    size_t e = doc.find('>', p);
    if (e == std::string::npos) break;
    std::string tag = doc.substr(p, e - p);
    p = e + 1;
    std::string name = Attr(tag, "name");
    std::string value = Attr(tag, "value");
    if (name.empty() || value.empty()) continue;  // connected input, no constant

    if (name == "base") {
      ParseFloats(value, &base_weight, 1);
    } else if (name == "base_color") {
      ParseFloats(value, out->base_color_factor, 3);
    } else if (name == "metalness") {
      ParseFloats(value, &out->metallic_factor, 1);
    } else if (name == "specular_roughness") {
      ParseFloats(value, &out->roughness_factor, 1);
    } else if (name == "specular_IOR") {
      ParseFloats(value, &out->ior, 1);
    } else if (name == "specular_anisotropy") {
      ParseFloats(value, &out->anisotropy, 1);
    } else if (name == "coat") {
      ParseFloats(value, &out->clearcoat, 1);
    } else if (name == "coat_roughness") {
      ParseFloats(value, &out->clearcoat_roughness, 1);
    } else if (name == "sheen") {
      ParseFloats(value, &sheen_weight, 1);
    } else if (name == "sheen_color") {
      ParseFloats(value, sheen_color, 3);
    } else if (name == "sheen_roughness") {
      ParseFloats(value, &out->sheen_roughness, 1);
    } else if (name == "subsurface") {
      ParseFloats(value, &out->subsurface, 1);
    } else if (name == "subsurface_color") {
      ParseFloats(value, out->subsurface_color, 3);
    } else if (name == "transmission") {
      ParseFloats(value, &out->transmission, 1);
    } else if (name == "emission") {
      ParseFloats(value, &emission_weight, 1);
    } else if (name == "emission_color") {
      ParseFloats(value, emission_color, 3);
    } else if (name == "thin_film_thickness") {
      ParseFloats(value, &thin_film_thickness, 1);
    }
  }

  for (int i = 0; i < 3; ++i) {
    out->base_color_factor[i] *= base_weight;
    out->sheen_color[i] = sheen_color[i] * sheen_weight;
    out->emissive_factor[i] = emission_color[i] * emission_weight;
  }
  if (thin_film_thickness > 0.0f) {
    out->iridescence = 1.0f;
    out->iridescence_thickness = thin_film_thickness;
  }
  if (out->transmission > 0.0f) out->alpha_mode = AlphaMode::kBlend;
  REC_INFO("materialx: loaded standard_surface from {}", path);
  return true;
}

}  // namespace rec::asset
