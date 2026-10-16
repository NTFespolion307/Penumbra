#pragma once

// Principled BSDF mapping 1:1 to glTF PBR and the material extensions in docs/GUIDE.md:
//   coat (GGX, IOR 1.5)  over  sheen (Charlie)  over  base = mix(dielectric, metal, metallic)
//   dielectric = specular reflection (GGX, exact Fresnel x KHR_materials_specular tint)
//              + (1 - albedo) * [ (1 - transmission) * Lambert diffuse + transmission * rough refraction ]
// Lower layers are attenuated by the upper layers' Fresnel / albedo (albedo scaling). Lobes are picked by
// estimated albedo at wo and combined by one-sample MIS (pdf = sum p_i pdf_i). Inside a transmissive
// material (wo below the surface) the BSDF is a plain dielectric interface.
// Local frame: shading normal = +z. eval returns f * |cos(wi)|.

#include <cstdint>

#include "core/shading/microfacet.h"

namespace pen {

enum class LobeKind : uint8_t { Diffuse, Glossy, Transmission };

struct BsdfInputs {
  Vec3f base_color{0.8f, 0.8f, 0.8f};
  float metallic = 0;
  float roughness = 0.5f;
  float transmission = 0;
  float ior = 1.5f;
  float specular = 1;
  Vec3f specular_color{1, 1, 1};
  float clearcoat = 0;
  float clearcoat_roughness = 0;
  Vec3f sheen_color{0, 0, 0};
  float sheen_roughness = 0;
  float anisotropy = 0;  // strength in [0,1]; direction is the frame's tangent
};

struct BsdfSample {
  Vec3f wi;
  Vec3f weight;  // f * |cos| / pdf
  float pdf = 0; // solid-angle pdf; 0 for delta lobes
  bool delta = false;
  LobeKind kind = LobeKind::Diffuse;
};

// Directional albedo of the Charlie sheen lobe for a white sheen colour (precomputed table).
float sheenAlbedo(float cosTheta, float roughness);

class PrincipledBsdf {
 public:
  PrincipledBsdf(const BsdfInputs& in, Vec3f wo);

  // f * |cos(wi)| summed over non-delta lobes, and the matching one-sample MIS pdf.
  Vec3f eval(Vec3f wo, Vec3f wi, float& pdf) const;
  bool sample(Vec3f wo, float uLobe, Vec2f u, BsdfSample& out) const;

  // True when every lobe that can be picked is a delta (mirror / smooth glass): skip light sampling.
  bool isDelta() const;

 private:
  enum Lobe { kDiffuse, kSheen, kSpec, kCoat, kTrans, kLobeCount };

  float baseAttenuation(float cosO, float cosI) const;
  Vec3f specFresnel(float cosOH) const;
  Vec3f evalOutside(Vec3f wo, Vec3f wi, float& pdf) const;
  Vec3f evalInside(Vec3f wo, Vec3f wi, float& pdf) const;
  bool sampleInside(Vec3f wo, float uLobe, Vec2f u, BsdfSample& out) const;

  BsdfInputs in_;
  GGX spec_, coat_;
  float eta_ = 1.5f;
  bool inside_ = false;
  bool spec_delta_ = false, coat_delta_ = false;
  float diel_albedo_ = 0;  // dielectric specular albedo estimate at wo, for diffuse/transmission scaling
  Vec3f spec_tint_{1, 1, 1};
  float prob_[kLobeCount] = {};
};

}  // namespace pen
