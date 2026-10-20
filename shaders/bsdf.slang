// Port of core/shading/bsdf.h / bsdf.cpp: the principled BSDF. The sheen albedo table is computed on the CPU
// (identical values) and read from a buffer.
#pragma once
#include "microfacet.slang"
#include "warp.slang"

static const uint kLobeDiffuse = 0;
static const uint kLobeGlossy = 1;
static const uint kLobeTransmission = 2;

static const float kCoatF0 = 0.04;
static const int kSheenN = 32;

[[vk::binding(20, 0)]] StructuredBuffer<float> g_sheen_table;

struct BsdfInputs {
  float3 base_color;
  float metallic;
  float roughness;
  float transmission;
  float ior;
  float specular;
  float3 specular_color;
  float clearcoat;
  float clearcoat_roughness;
  float3 sheen_color;
  float sheen_roughness;
  float anisotropy;
};

struct BsdfSample {
  float3 wi;
  float3 weight;
  float pdf;
  bool delta;
  uint kind;
};

float charlieD(float cosH, float roughness) {
  float alpha = max(roughness * roughness, 1e-3);
  float invA = 1 / alpha;
  float sinH = safeSqrt(1 - cosH * cosH);
  return (2 + invA) * pow(sinH, invA) * kInv2Pi;
}

float ashikhminV(float cosO, float cosI) { return clampf(1 / (4 * (cosI + cosO - cosI * cosO)), 0.0, 1.0); }

float sheenAlbedo(float cosTheta, float roughness) {
  float x = clampf(cosTheta, 0.0, 1.0) * (kSheenN - 1), y = clampf(roughness, 0.0, 1.0) * (kSheenN - 1);
  int x0 = min(int(x), kSheenN - 2), y0 = min(int(y), kSheenN - 2);
  float fx = x - x0, fy = y - y0;
  float a = lerpf(g_sheen_table[y0 * kSheenN + x0], g_sheen_table[y0 * kSheenN + x0 + 1], fx);
  float b = lerpf(g_sheen_table[(y0 + 1) * kSheenN + x0], g_sheen_table[(y0 + 1) * kSheenN + x0 + 1], fx);
  return clampf(lerpf(a, b, fy), 0.0, 1.0);
}

static const int kDiffuse = 0;
static const int kSheen = 1;
static const int kSpec = 2;
static const int kCoat = 3;
static const int kTrans = 4;
static const int kLobeCount = 5;

struct PrincipledBsdf {
  BsdfInputs inp;
  GGX spec;
  GGX coat;
  float eta;
  bool inside;
  bool spec_delta;
  bool coat_delta;
  float diel_albedo;
  float3 spec_tint;
  float prob[5];

  bool isDelta() {
    if (inside) return spec_delta;
    return prob[kDiffuse] == 0 && prob[kSheen] == 0 && (prob[kSpec] == 0 || spec_delta) &&
           (prob[kTrans] == 0 || spec_delta) && (prob[kCoat] == 0 || coat_delta);
  }

  float baseAttenuation(float cosO, float cosI) {
    float c = (1 - inp.clearcoat * fresnelSchlick1(kCoatF0, cosO)) * (1 - inp.clearcoat * fresnelSchlick1(kCoatF0, cosI));
    float s = maxComponent(inp.sheen_color);
    if (s <= 0) return c;
    float sheen = min(1 - s * sheenAlbedo(cosO, inp.sheen_roughness), 1 - s * sheenAlbedo(cosI, inp.sheen_roughness));
    return c * sheen;
  }

  float3 specFresnel(float cosOH) {
    float m = inp.metallic;
    return spec_tint * ((1 - m) * fresnelDielectric(cosOH, eta)) + fresnelSchlick3(inp.base_color, cosOH) * m;
  }

  float3 evalOutside(float3 wo, float3 wi, out float pdf) {
    float cosO = wo.z, cosI = abs(wi.z);
    float atten = baseAttenuation(cosO, cosI);
    float3 f = float3(0, 0, 0);
    pdf = 0;
    if (wi.z > 0) {
      float cosPdf = cosI * kInvPi;
      if (prob[kDiffuse] > 0) {
        f += inp.base_color * ((1 - inp.metallic) * (1 - inp.transmission) * (1 - diel_albedo) * kInvPi * atten);
        pdf += prob[kDiffuse] * cosPdf;
      }
      float3 h = normalizeSafe(wo + wi);
      if (prob[kSheen] > 0) {
        float coatOnly = (1 - inp.clearcoat * fresnelSchlick1(kCoatF0, cosO)) *
                         (1 - inp.clearcoat * fresnelSchlick1(kCoatF0, cosI));
        f += inp.sheen_color * (charlieD(h.z, inp.sheen_roughness) * ashikhminV(cosO, cosI) * coatOnly);
        pdf += prob[kSheen] * cosPdf;
      }
      if (prob[kSpec] > 0 && !spec_delta) {
        float dotOH = dot(wo, h);
        if (dotOH > 0) {
          f += specFresnel(dotOH) * (spec.D(h) * spec.G(wo, wi) / (4 * cosO * cosI) * atten);
          pdf += prob[kSpec] * spec.Dvisible(wo, h) / (4 * dotOH);
        }
      }
      if (prob[kCoat] > 0 && !coat_delta) {
        float dotOH = dot(wo, h);
        if (dotOH > 0) {
          f += float3(1, 1, 1) * (inp.clearcoat * fresnelSchlick1(kCoatF0, dotOH) * coat.D(h) * coat.G(wo, wi) / (4 * cosO * cosI));
          pdf += prob[kCoat] * coat.Dvisible(wo, h) / (4 * dotOH);
        }
      }
    } else if (prob[kTrans] > 0 && !spec_delta) {
      float etap = eta;
      float3 wm = wi * etap + wo;
      if (lengthSquared(wm) == 0) return float3(0, 0, 0);
      wm = normalizeSafe(wm);
      if (wm.z < 0) wm = -wm;
      if (dot(wm, wi) * wi.z < 0 || dot(wm, wo) * wo.z < 0) return float3(0, 0, 0);
      float F = fresnelDielectric(dot(wo, wm), eta);
      float denom = sqr(dot(wi, wm) + dot(wo, wm) / etap);
      float ft = spec.D(wm) * (1 - F) * spec.G(wo, wi) * abs(dot(wi, wm) * dot(wo, wm) / (denom * wi.z * wo.z)) / sqr(etap);
      f += inp.base_color * ((1 - inp.metallic) * inp.transmission * ft * atten);
      pdf += prob[kTrans] * spec.Dvisible(wo, wm) * abs(dot(wi, wm)) / denom;
    }
    return f * cosI;
  }

  float3 evalInside(float3 wo, float3 wi, out float pdf) {
    pdf = 0;
    if (spec_delta) return float3(0, 0, 0);
    float cosO = wo.z, cosI = wi.z;
    bool refl = cosO * cosI > 0;
    float etap = refl ? 1.0 : (cosO > 0 ? eta : 1 / eta);
    float3 wm = wi * etap + wo;
    if (lengthSquared(wm) == 0) return float3(0, 0, 0);
    wm = normalizeSafe(wm);
    if (wm.z < 0) wm = -wm;
    if (dot(wm, wi) * cosI < 0 || dot(wm, wo) * cosO < 0) return float3(0, 0, 0);
    float R = fresnelDielectric(dot(wo, wm), eta), T = 1 - R;
    if (refl) {
      float f = spec.D(wm) * spec.G(wo, wi) * R / abs(4 * cosI * cosO);
      pdf = spec.Dvisible(wo, wm) / (4 * abs(dot(wo, wm))) * R / (R + T);
      return float3(1, 1, 1) * (f * abs(cosI));
    }
    float denom = sqr(dot(wi, wm) + dot(wo, wm) / etap);
    float ft = spec.D(wm) * T * spec.G(wo, wi) * abs(dot(wi, wm) * dot(wo, wm) / (denom * cosI * cosO)) / sqr(etap);
    pdf = spec.Dvisible(wo, wm) * abs(dot(wi, wm)) / denom * T / (R + T);
    return float3(1, 1, 1) * (ft * abs(cosI));
  }

  float3 eval(float3 wo, float3 wi, out float pdf) {
    pdf = 0;
    if (wo.z == 0 || wi.z == 0) return float3(0, 0, 0);
    return inside ? evalInside(wo, wi, pdf) : evalOutside(wo, wi, pdf);
  }

  bool sampleInside(float3 wo, float uLobe, float2 u, inout BsdfSample o) {
    o.kind = kLobeTransmission;
    if (spec_delta) {
      float R = fresnelDielectric(wo.z, eta), T = 1 - R;
      o.delta = true;
      o.pdf = 0;
      if (uLobe < R / (R + T)) {
        o.wi = float3(-wo.x, -wo.y, wo.z);
        o.weight = float3(1, 1, 1);
        o.kind = kLobeGlossy;
        return true;
      }
      float etap;
      if (!refractDir(wo, float3(0, 0, 1), eta, etap, o.wi)) return false;
      o.weight = float3(1, 1, 1) * (1 / sqr(etap));
      return true;
    }
    float3 wm = spec.sampleVisible(wo, u);
    float R = fresnelDielectric(dot(wo, wm), eta), T = 1 - R;
    if (uLobe < R / (R + T)) {
      o.wi = reflectDir(wo, wm);
      if (!sameHemisphere(wo, o.wi)) return false;
      o.kind = kLobeGlossy;
    } else {
      float etap;
      if (!refractDir(wo, wm, eta, etap, o.wi) || sameHemisphere(wo, o.wi) || o.wi.z == 0) return false;
    }
    float3 f = evalInside(wo, o.wi, o.pdf);
    if (o.pdf <= 0) return false;
    o.weight = f / o.pdf;
    o.delta = false;
    return true;
  }

  bool sample(float3 wo, float uLobe, float2 u, out BsdfSample o) {
    o.wi = float3(0, 0, 0);
    o.weight = float3(0, 0, 0);
    o.pdf = 0;
    o.delta = false;
    o.kind = kLobeDiffuse;
    if (wo.z == 0) return false;
    if (inside) return sampleInside(wo, uLobe, u, o);

    int lobe = kLobeCount - 1;
    float acc = 0;
    for (int i = 0; i < kLobeCount; ++i) {
      acc += prob[i];
      if (uLobe < acc) {
        lobe = i;
        break;
      }
    }
    while (lobe > 0 && prob[lobe] == 0) --lobe;
    float cosO = wo.z;

    if (lobe == kDiffuse || lobe == kSheen) {
      o.wi = sampleCosineHemisphere(u);
      o.kind = kLobeDiffuse;
    } else if (lobe == kSpec) {
      if (spec_delta) {
        o.wi = float3(-wo.x, -wo.y, wo.z);
        o.weight = specFresnel(cosO) * (baseAttenuation(cosO, cosO) / prob[kSpec]);
        o.pdf = 0;
        o.delta = true;
        o.kind = kLobeGlossy;
        return true;
      }
      o.wi = reflectDir(wo, spec.sampleVisible(wo, u));
      o.kind = kLobeGlossy;
    } else if (lobe == kCoat) {
      if (coat_delta) {
        o.wi = float3(-wo.x, -wo.y, wo.z);
        o.weight = float3(1, 1, 1) * (inp.clearcoat * fresnelSchlick1(kCoatF0, cosO) / prob[kCoat]);
        o.pdf = 0;
        o.delta = true;
        o.kind = kLobeGlossy;
        return true;
      }
      o.wi = reflectDir(wo, coat.sampleVisible(wo, u));
      o.kind = kLobeGlossy;
    } else {
      o.kind = kLobeTransmission;
      if (spec_delta) {
        float etap;
        if (!refractDir(wo, float3(0, 0, 1), eta, etap, o.wi)) return false;
        float F = fresnelDielectric(cosO, eta);
        o.weight = inp.base_color * ((1 - inp.metallic) * inp.transmission * (1 - F) *
                                     baseAttenuation(cosO, abs(o.wi.z)) / sqr(etap) / prob[kTrans]);
        o.pdf = 0;
        o.delta = true;
        return true;
      }
      float3 wm = spec.sampleVisible(wo, u);
      float etap;
      if (!refractDir(wo, wm, eta, etap, o.wi) || o.wi.z >= 0) return false;
    }
    if (o.wi.z == 0) return false;
    if (o.kind != kLobeTransmission && o.wi.z < 0) return false;
    float3 f = evalOutside(wo, o.wi, o.pdf);
    if (o.pdf <= 0) return false;
    o.weight = f / o.pdf;
    o.delta = false;
    return true;
  }
};

PrincipledBsdf makeBsdf(BsdfInputs inp, float3 wo) {
  PrincipledBsdf b;
  b.inp = inp;
  b.inp.metallic = clampf(inp.metallic, 0, 1);
  b.inp.transmission = clampf(inp.transmission, 0, 1);
  b.inp.clearcoat = clampf(inp.clearcoat, 0, 1);
  b.eta = clampf(inp.ior, 1.001, 4.0);
  for (int i = 0; i < kLobeCount; ++i) b.prob[i] = 0;
  b.diel_albedo = 0;

  float alpha = sqr(clampf(inp.roughness, 0, 1));
  float aniso = clampf(inp.anisotropy, 0, 1);
  b.spec = makeGGX(max(lerpf(alpha, 1.0, aniso * aniso), 1e-4), max(alpha, 1e-4));
  b.spec_delta = b.spec.isDelta();
  float ca = max(sqr(clampf(inp.clearcoat_roughness, 0, 1)), 1e-4);
  b.coat = makeGGX(ca, ca);
  b.coat_delta = b.coat.isDelta();

  b.spec_tint = min(inp.specular_color * clampf(inp.specular, 0, 1), float3(1, 1, 1));
  b.inside = wo.z < 0 && b.inp.transmission > 0 && b.inp.metallic < 1;
  if (b.inside) return b;

  float cosO = abs(wo.z);
  b.diel_albedo = fresnelDielectric(cosO, b.eta) * maxComponent(b.spec_tint);
  float3 metalF = fresnelSchlick3(b.inp.base_color, cosO);
  float coatF = b.inp.clearcoat * fresnelSchlick1(kCoatF0, cosO);
  float sheenE = maxComponent(b.inp.sheen_color) * sheenAlbedo(cosO, b.inp.sheen_roughness);
  float atten = (1 - coatF) * (1 - sheenE);
  float m = b.inp.metallic, t = b.inp.transmission, base = luminance(b.inp.base_color);

  b.prob[kSpec] = atten * ((1 - m) * b.diel_albedo + m * luminance(metalF));
  b.prob[kDiffuse] = atten * (1 - m) * (1 - t) * (1 - b.diel_albedo) * base;
  b.prob[kTrans] = atten * (1 - m) * t * (1 - b.diel_albedo) * base;
  b.prob[kCoat] = coatF;
  b.prob[kSheen] = (1 - coatF) * sheenE;
  float sum = 0;
  for (int i = 0; i < kLobeCount; ++i) sum += b.prob[i];
  if (sum > 0) {
    for (int i = 0; i < kLobeCount; ++i) b.prob[i] /= sum;
  } else {
    b.prob[kDiffuse] = 1;
  }
  return b;
}
