#include "core/output/tonemap.h"

#include <algorithm>
#include <cmath>

#include "core/image/image.h"

namespace pen {

namespace {

// m * v for a 3x3 matrix given row by row.
Vec3f mul(const float (&m)[9], Vec3f v) {
  return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z,
          m[6] * v.x + m[7] * v.y + m[8] * v.z};
}

Vec3f clamp01(Vec3f c) {
  return {std::clamp(c.x, 0.0f, 1.0f), std::clamp(c.y, 0.0f, 1.0f), std::clamp(c.z, 0.0f, 1.0f)};
}

Vec3f srgbEncode(Vec3f c) {
  const Vec3f k = clamp01(c);
  return {linearToSrgb(k.x), linearToSrgb(k.y), linearToSrgb(k.z)};
}

// AgX as in Blender 4.x: the Rec. 2020 inset and outset matrices as published (Filament, three.js), the curve as
// Blender 4.2 draws it (below).
constexpr float kRec709ToRec2020[9] = {0.6274f, 0.3293f, 0.0433f, 0.0691f, 0.9195f, 0.0113f, 0.0164f, 0.0880f, 0.8956f};
constexpr float kRec2020ToRec709[9] = {1.6605f, -0.5876f, -0.0728f, -0.1246f, 1.1329f, -0.0083f, -0.0182f, -0.1006f, 1.1187f};
constexpr float kAgxInset[9] = {0.856627153315983f,  0.0951212405381588f, 0.0482516061458583f,
                                0.137318972929847f,  0.761241990602591f,  0.101439036467562f,
                                0.11189821299995f,   0.0767994186031903f, 0.811302368396859f};
constexpr float kAgxOutset[9] = {1.1271005818144368f,  -0.11060664309660323f, -0.016493938717834573f,
                                 -0.1413297634984383f, 1.157823702216272f,    -0.016493938717834257f,
                                 -0.14132976349843826f, -0.11060664309660294f, 1.2519364065950405f};
constexpr float kAgxMinEv = -12.47393f, kAgxMaxEv = 4.026069f;

// The sigmoid, measured from Blender 4.2 at every half EV from -10 to +8 around 0.18: the value before the outset
// matrix that gives Blender's grey. The widely copied 6th-order polynomial is a different curve (18% grey 0.50
// instead of Blender's 0.46, mean 6.8 8-bit steps off; D-126).
constexpr float kAgxCurve[37] = {
    0.00000f, 0.00347f, 0.00707f, 0.01165f, 0.01699f, 0.02318f, 0.03029f, 0.03851f, 0.04808f, 0.05931f,
    0.07253f, 0.08812f, 0.10657f, 0.12853f, 0.15475f, 0.18620f, 0.22393f, 0.26937f, 0.32394f, 0.38757f,
    0.45861f, 0.53134f, 0.59993f, 0.66247f, 0.71853f, 0.76822f, 0.81150f, 0.84955f, 0.88304f, 0.91251f,
    0.93850f, 0.96147f, 0.98184f, 0.99940f, 1.00000f, 1.00000f, 1.00000f};

// A table over EV from -10 to +8 around 0.18, every half EV, read with linear interpolation.
float halfEvTable(const float (&t)[37], float ev) {
  const float x = (ev + 10) * 2;
  if (x <= 0) return t[0];
  if (x >= 36) return t[36];
  const int i = static_cast<int>(x);
  const float f = x - i;
  return t[i] * (1 - f) + t[i + 1] * f;
}

Vec3f agx(Vec3f c) {
  c = mul(kAgxInset, mul(kRec709ToRec2020, c));
  for (int i = 0; i < 3; ++i) {
    const float ev = std::clamp(std::log2(std::max(c[i], 1e-10f)), kAgxMinEv, kAgxMaxEv);
    c[i] = halfEvTable(kAgxCurve, ev - std::log2(0.18f));
  }
  c = mul(kAgxOutset, c);
  for (int i = 0; i < 3; ++i) c[i] = std::pow(std::max(c[i], 0.0f), 2.2f);  // the curve's output is 2.2-encoded
  return srgbEncode(mul(kRec2020ToRec709, c));
}

// Blender's Filmic (Medium Contrast), its grey response measured from Blender 4.2 at every half EV from -10 to
// +8 around 0.18 (display-encoded), applied per channel. Blender also desaturates bright colours with a 3D LUT;
// that part is not reproduced.
constexpr float kFilmicGrey[37] = {
    0.00002f, 0.00191f, 0.00430f, 0.00737f, 0.01125f, 0.01614f, 0.02235f, 0.03015f, 0.03996f, 0.05222f,
    0.06748f, 0.08635f, 0.10948f, 0.13761f, 0.17134f, 0.21128f, 0.25768f, 0.31054f, 0.36936f, 0.43311f,
    0.50021f, 0.56774f, 0.63444f, 0.69818f, 0.75708f, 0.80981f, 0.85557f, 0.89410f, 0.92558f, 0.95053f,
    0.96968f, 0.98378f, 0.99365f, 1.00000f, 1.00000f, 1.00000f, 1.00000f};

float filmic(float v) { return halfEvTable(kFilmicGrey, std::log2(std::max(v, 1e-20f) / 0.18f)); }

// ACES: Stephen Hill's fit (BakingLab, MIT) of the reference rendering and sRGB output transforms.
constexpr float kAcesIn[9] = {0.59719f, 0.35458f, 0.04823f, 0.07600f, 0.90834f, 0.01566f, 0.02840f, 0.13383f, 0.83777f};
constexpr float kAcesOut[9] = {1.60475f, -0.53108f, -0.07367f, -0.10208f, 1.10813f, -0.00605f, -0.00327f, -0.07276f, 1.07602f};

Vec3f aces(Vec3f c) {
  c = mul(kAcesIn, c);
  for (int i = 0; i < 3; ++i) {
    const float v = c[i];
    c[i] = (v * (v + 0.0245786f) - 0.000090537f) / (v * (0.983729f * v + 0.4329510f) + 0.238081f);
  }
  return srgbEncode(mul(kAcesOut, c));
}

}  // namespace

Vec3f tonemap(Tonemap mode, Vec3f linear) {
  const Vec3f c{std::max(linear.x, 0.0f), std::max(linear.y, 0.0f), std::max(linear.z, 0.0f)};
  switch (mode) {
    case Tonemap::Agx: return agx(c);
    case Tonemap::Filmic: return {filmic(c.x), filmic(c.y), filmic(c.z)};
    case Tonemap::Aces: return aces(c);
    case Tonemap::None: break;
  }
  return srgbEncode(c);
}

}  // namespace pen
