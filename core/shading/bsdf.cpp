#include "core/shading/bsdf.h"

#include <algorithm>
#include <array>

#include "core/sampling/warp.h"

namespace pen {

namespace {

constexpr float kCoatF0 = 0.04f;  // clearcoat IOR 1.5

PEN_HD inline float charlieD(float cosH, float roughness) {
  const float alpha = std::fmax(roughness * roughness, 1e-3f);
  const float invA = 1 / alpha;
  const float sinH = safeSqrt(1 - cosH * cosH);
  return (2 + invA) * std::pow(sinH, invA) * kInv2Pi;
}

PEN_HD inline float ashikhminV(float cosO, float cosI) {
  return clamp(1 / (4 * (cosI + cosO - cosI * cosO)), 0.0f, 1.0f);
}

constexpr int kSheenN = 32;

const std::array<float, kSheenN * kSheenN>& sheenTable() {
  // E(cos, roughness) = integral of D * V * cos over the hemisphere, by stratified cosine sampling
  // (deterministic, identical on every machine).
  static const std::array<float, kSheenN * kSheenN> table = [] {
    std::array<float, kSheenN * kSheenN> t{};
    constexpr int kStrata = 64;
    for (int ri = 0; ri < kSheenN; ++ri) {
      const float rough = ri / float(kSheenN - 1);
      for (int ci = 0; ci < kSheenN; ++ci) {
        const float cosO = std::fmax(ci / float(kSheenN - 1), 1e-3f);
        const Vec3f wo{safeSqrt(1 - cosO * cosO), 0, cosO};
        double sum = 0;
        for (int a = 0; a < kStrata; ++a) {
          for (int b = 0; b < kStrata; ++b) {
            const Vec3f wi = sampleCosineHemisphere({(a + 0.5f) / kStrata, (b + 0.5f) / kStrata});
            if (wi.z <= 0) continue;
            const Vec3f h = normalize(wo + wi);
            // f * cos / pdf with pdf = cos / pi
            sum += charlieD(h.z, rough) * ashikhminV(cosO, wi.z) * kPi;
          }
        }
        t[ri * kSheenN + ci] = static_cast<float>(sum / (kStrata * kStrata));
      }
    }
    return t;
  }();
  return table;
}

}  // namespace

const float* sheenAlbedoTable() { return sheenTable().data(); }

float sheenAlbedo(float cosTheta, float roughness) {
  const auto& t = sheenTable();
  const float x = clamp(cosTheta, 0.0f, 1.0f) * (kSheenN - 1), y = clamp(roughness, 0.0f, 1.0f) * (kSheenN - 1);
  const int x0 = std::min(static_cast<int>(x), kSheenN - 2), y0 = std::min(static_cast<int>(y), kSheenN - 2);
  const float fx = x - x0, fy = y - y0;
  const float a = lerp(t[y0 * kSheenN + x0], t[y0 * kSheenN + x0 + 1], fx);
  const float b = lerp(t[(y0 + 1) * kSheenN + x0], t[(y0 + 1) * kSheenN + x0 + 1], fx);
  return clamp(lerp(a, b, fy), 0.0f, 1.0f);
}

PrincipledBsdf::PrincipledBsdf(const BsdfInputs& in, Vec3f wo) : in_(in) {
  in_.metallic = clamp(in_.metallic, 0, 1);
  in_.transmission = clamp(in_.transmission, 0, 1);
  in_.clearcoat = clamp(in_.clearcoat, 0, 1);
  eta_ = clamp(in_.ior, 1.001f, 4.0f);

  const float alpha = sqr(clamp(in_.roughness, 0, 1));
  const float aniso = clamp(in_.anisotropy, 0, 1);
  spec_.ax = std::fmax(lerp(alpha, 1.0f, aniso * aniso), 1e-4f);
  spec_.ay = std::fmax(alpha, 1e-4f);
  spec_delta_ = spec_.isDelta();
  const float ca = std::fmax(sqr(clamp(in_.clearcoat_roughness, 0, 1)), 1e-4f);
  coat_.ax = coat_.ay = ca;
  coat_delta_ = coat_.isDelta();

  spec_tint_ = vmin(in_.specular_color * clamp(in_.specular, 0, 1), Vec3f(1));
  inside_ = wo.z < 0 && in_.transmission > 0 && in_.metallic < 1;
  if (inside_) return;

  const float cosO = std::fabs(wo.z);
  diel_albedo_ = fresnelDielectric(cosO, eta_) * maxComponent(spec_tint_);
  const Vec3f metalF = fresnelSchlick(in_.base_color, cosO);
  const float coatF = in_.clearcoat * fresnelSchlick(kCoatF0, cosO);
  const float sheenE = maxComponent(in_.sheen_color) * sheenAlbedo(cosO, in_.sheen_roughness);
  const float atten = (1 - coatF) * (1 - sheenE);
  const float m = in_.metallic, t = in_.transmission, base = luminance(in_.base_color);

  prob_[kSpec] = atten * ((1 - m) * diel_albedo_ + m * luminance(metalF));
  prob_[kDiffuse] = atten * (1 - m) * (1 - t) * (1 - diel_albedo_) * base;
  prob_[kTrans] = atten * (1 - m) * t * (1 - diel_albedo_) * base;
  prob_[kCoat] = coatF;
  prob_[kSheen] = (1 - coatF) * sheenE;
  float sum = 0;
  for (float p : prob_) sum += p;
  if (sum > 0) {
    for (float& p : prob_) p /= sum;
  } else {
    prob_[kDiffuse] = 1;
  }
}

bool PrincipledBsdf::isDelta() const {
  if (inside_) return spec_delta_;
  return prob_[kDiffuse] == 0 && prob_[kSheen] == 0 && (prob_[kSpec] == 0 || spec_delta_) &&
         (prob_[kTrans] == 0 || spec_delta_) && (prob_[kCoat] == 0 || coat_delta_);
}

float PrincipledBsdf::baseAttenuation(float cosO, float cosI) const {
  const float coat = (1 - in_.clearcoat * fresnelSchlick(kCoatF0, cosO)) *
                     (1 - in_.clearcoat * fresnelSchlick(kCoatF0, cosI));
  const float s = maxComponent(in_.sheen_color);
  if (s <= 0) return coat;
  const float sheen = std::fmin(1 - s * sheenAlbedo(cosO, in_.sheen_roughness),
                                1 - s * sheenAlbedo(cosI, in_.sheen_roughness));
  return coat * sheen;
}

Vec3f PrincipledBsdf::specFresnel(float cosOH) const {
  const float m = in_.metallic;
  return spec_tint_ * ((1 - m) * fresnelDielectric(cosOH, eta_)) + fresnelSchlick(in_.base_color, cosOH) * m;
}

Vec3f PrincipledBsdf::eval(Vec3f wo, Vec3f wi, float& pdf) const {
  pdf = 0;
  if (wo.z == 0 || wi.z == 0) return {};
  return inside_ ? evalInside(wo, wi, pdf) : evalOutside(wo, wi, pdf);
}

Vec3f PrincipledBsdf::evalOutside(Vec3f wo, Vec3f wi, float& pdf) const {
  const float cosO = wo.z, cosI = std::fabs(wi.z);
  const float atten = baseAttenuation(cosO, cosI);
  Vec3f f(0);
  pdf = 0;
  if (wi.z > 0) {
    // Diffuse and sheen (cosine-sampled).
    const float cosPdf = cosI * kInvPi;
    if (prob_[kDiffuse] > 0) {
      f += in_.base_color * ((1 - in_.metallic) * (1 - in_.transmission) * (1 - diel_albedo_) * kInvPi * atten);
      pdf += prob_[kDiffuse] * cosPdf;
    }
    const Vec3f h = normalize(wo + wi);
    if (prob_[kSheen] > 0) {
      const float coatOnly = (1 - in_.clearcoat * fresnelSchlick(kCoatF0, cosO)) *
                             (1 - in_.clearcoat * fresnelSchlick(kCoatF0, cosI));
      f += in_.sheen_color * (charlieD(h.z, in_.sheen_roughness) * ashikhminV(cosO, cosI) * coatOnly);
      pdf += prob_[kSheen] * cosPdf;
    }
    if (prob_[kSpec] > 0 && !spec_delta_) {
      const float dotOH = dot(wo, h);
      if (dotOH > 0) {
        f += specFresnel(dotOH) * (spec_.D(h) * spec_.G(wo, wi) / (4 * cosO * cosI) * atten);
        pdf += prob_[kSpec] * spec_.Dvisible(wo, h) / (4 * dotOH);
      }
    }
    if (prob_[kCoat] > 0 && !coat_delta_) {
      const float dotOH = dot(wo, h);
      if (dotOH > 0) {
        f += Vec3f(in_.clearcoat * fresnelSchlick(kCoatF0, dotOH) * coat_.D(h) * coat_.G(wo, wi) / (4 * cosO * cosI));
        pdf += prob_[kCoat] * coat_.Dvisible(wo, h) / (4 * dotOH);
      }
    }
  } else if (prob_[kTrans] > 0 && !spec_delta_) {
    // Rough refraction into the material (pbrt-v4 DielectricBxDF, radiance mode).
    const float etap = eta_;
    Vec3f wm = wi * etap + wo;
    if (lengthSquared(wm) == 0) return {};
    wm = normalize(wm);
    if (wm.z < 0) wm = -wm;
    if (dot(wm, wi) * wi.z < 0 || dot(wm, wo) * wo.z < 0) return {};
    const float F = fresnelDielectric(dot(wo, wm), eta_);
    const float denom = sqr(dot(wi, wm) + dot(wo, wm) / etap);
    const float ft = spec_.D(wm) * (1 - F) * spec_.G(wo, wi) *
                     std::fabs(dot(wi, wm) * dot(wo, wm) / (denom * wi.z * wo.z)) / sqr(etap);
    f += in_.base_color * ((1 - in_.metallic) * in_.transmission * ft * atten);
    pdf += prob_[kTrans] * spec_.Dvisible(wo, wm) * std::fabs(dot(wi, wm)) / denom;
  }
  return f * cosI;
}

Vec3f PrincipledBsdf::evalInside(Vec3f wo, Vec3f wi, float& pdf) const {
  pdf = 0;
  if (spec_delta_) return {};
  const float cosO = wo.z, cosI = wi.z;
  const bool reflect = cosO * cosI > 0;
  const float etap = reflect ? 1.0f : (cosO > 0 ? eta_ : 1 / eta_);
  Vec3f wm = wi * etap + wo;
  if (lengthSquared(wm) == 0) return {};
  wm = normalize(wm);
  if (wm.z < 0) wm = -wm;
  if (dot(wm, wi) * cosI < 0 || dot(wm, wo) * cosO < 0) return {};
  const float R = fresnelDielectric(dot(wo, wm), eta_), T = 1 - R;
  if (reflect) {
    const float f = spec_.D(wm) * spec_.G(wo, wi) * R / std::fabs(4 * cosI * cosO);
    pdf = spec_.Dvisible(wo, wm) / (4 * std::fabs(dot(wo, wm))) * R / (R + T);
    return Vec3f(f * std::fabs(cosI));
  }
  const float denom = sqr(dot(wi, wm) + dot(wo, wm) / etap);
  const float ft = spec_.D(wm) * T * spec_.G(wo, wi) * std::fabs(dot(wi, wm) * dot(wo, wm) / (denom * cosI * cosO)) /
                   sqr(etap);
  pdf = spec_.Dvisible(wo, wm) * std::fabs(dot(wi, wm)) / denom * T / (R + T);
  return Vec3f(ft * std::fabs(cosI));
}

bool PrincipledBsdf::sample(Vec3f wo, float uLobe, Vec2f u, BsdfSample& out) const {
  if (wo.z == 0) return false;
  if (inside_) return sampleInside(wo, uLobe, u, out);

  int lobe = kLobeCount - 1;
  float acc = 0;
  for (int i = 0; i < kLobeCount; ++i) {
    acc += prob_[i];
    if (uLobe < acc) {
      lobe = i;
      break;
    }
  }
  while (lobe > 0 && prob_[lobe] == 0) --lobe;
  const float cosO = wo.z;

  switch (lobe) {
    case kDiffuse:
    case kSheen:
      out.wi = sampleCosineHemisphere(u);
      out.kind = LobeKind::Diffuse;
      break;
    case kSpec:
      if (spec_delta_) {
        out.wi = {-wo.x, -wo.y, wo.z};
        out.weight = specFresnel(cosO) * (baseAttenuation(cosO, cosO) / prob_[kSpec]);
        out.pdf = 0;
        out.delta = true;
        out.kind = LobeKind::Glossy;
        return true;
      }
      out.wi = reflect(wo, spec_.sampleVisible(wo, u));
      out.kind = LobeKind::Glossy;
      break;
    case kCoat:
      if (coat_delta_) {
        out.wi = {-wo.x, -wo.y, wo.z};
        out.weight = Vec3f(in_.clearcoat * fresnelSchlick(kCoatF0, cosO) / prob_[kCoat]);
        out.pdf = 0;
        out.delta = true;
        out.kind = LobeKind::Glossy;
        return true;
      }
      out.wi = reflect(wo, coat_.sampleVisible(wo, u));
      out.kind = LobeKind::Glossy;
      break;
    case kTrans:
    default: {
      out.kind = LobeKind::Transmission;
      if (spec_delta_) {
        float etap = 1;
        if (!refract(wo, Vec3f(0, 0, 1), eta_, etap, out.wi)) return false;
        const float F = fresnelDielectric(cosO, eta_);
        out.weight = in_.base_color * ((1 - in_.metallic) * in_.transmission * (1 - F) *
                                       baseAttenuation(cosO, std::fabs(out.wi.z)) / sqr(etap) / prob_[kTrans]);
        out.pdf = 0;
        out.delta = true;
        return true;
      }
      const Vec3f wm = spec_.sampleVisible(wo, u);
      float etap = 1;
      if (!refract(wo, wm, eta_, etap, out.wi) || out.wi.z >= 0) return false;
      break;
    }
  }
  if (out.wi.z == 0) return false;
  if (out.kind != LobeKind::Transmission && out.wi.z < 0) return false;
  const Vec3f f = evalOutside(wo, out.wi, out.pdf);
  if (out.pdf <= 0) return false;
  out.weight = f / out.pdf;
  out.delta = false;
  return true;
}

bool PrincipledBsdf::sampleInside(Vec3f wo, float uLobe, Vec2f u, BsdfSample& out) const {
  out.kind = LobeKind::Transmission;
  if (spec_delta_) {
    const float R = fresnelDielectric(wo.z, eta_), T = 1 - R;
    out.delta = true;
    out.pdf = 0;
    if (uLobe < R / (R + T)) {
      out.wi = {-wo.x, -wo.y, wo.z};
      out.weight = Vec3f(1);
      out.kind = LobeKind::Glossy;
      return true;
    }
    float etap = 1;
    if (!refract(wo, Vec3f(0, 0, 1), eta_, etap, out.wi)) return false;
    out.weight = Vec3f(1 / sqr(etap));
    return true;
  }
  const Vec3f wm = spec_.sampleVisible(wo, u);
  const float R = fresnelDielectric(dot(wo, wm), eta_), T = 1 - R;
  if (uLobe < R / (R + T)) {
    out.wi = reflect(wo, wm);
    if (!sameHemisphere(wo, out.wi)) return false;
    out.kind = LobeKind::Glossy;
  } else {
    float etap = 1;
    if (!refract(wo, wm, eta_, etap, out.wi) || sameHemisphere(wo, out.wi) || out.wi.z == 0) return false;
  }
  const Vec3f f = evalInside(wo, out.wi, out.pdf);
  if (out.pdf <= 0) return false;
  out.weight = f / out.pdf;
  out.delta = false;
  return true;
}

}  // namespace pen
