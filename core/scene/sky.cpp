#include "core/scene/sky.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <thread>
#include <vector>

#include "core/system/sysinfo.h"

namespace pen {

namespace {

constexpr double kPiD = 3.14159265358979323846;
constexpr double kEarthRadius = 6360e3;       // m
constexpr double kAtmosphereRadius = 6420e3;  // 60 km of atmosphere
constexpr double kRayleighHeight = 8000, kMieHeight = 1200;
constexpr double kMieScatter = 2e-5, kMieExtinction = 2e-5 * 1.11, kMieG = 0.76;
constexpr int kLambdas = 21;  // 380, 400, ..., 780 nm
constexpr int kViewSteps = 64, kLightSteps = 32;

// Step k of n along a ray of length t, spaced as t (k / n)^2: dense near the start, where the density falls
// fastest (an even 7.5 km step spans six haze scale heights; the haze depth towards the sun came out 30% low and
// the horizon too bright and blue). Returns the step's midpoint distance and its length.
void step(double t, int k, int n, double& mid, double& len) {
  const double a = double(k) / n, b = double(k + 1) / n, m = (k + 0.5) / n;
  mid = t * m * m;
  len = t * (b * b - a * a);
}
// Calibration to Blender's Nishita sky at the same settings (M14 gate, D-120).
constexpr double kSkyScale = 1.0;

double lambdaNm(int i) { return 380.0 + 20.0 * i; }

// Rayleigh scattering of air at sea level (per metre): 8 pi^3 (n^2 - 1)^2 / (3 N lambda^4) times the King
// factor (depolarization 0.035), with the refractive index of air from Peck and Reeder (1972).
double rayleighBeta(double nm) {
  const double s2 = std::pow(1000.0 / nm, 2);  // (1 / lambda in um)^2
  const double nm1 = 1e-8 * (8060.51 + 2480990.0 / (132.274 - s2) + 17455.7 / (39.32957 - s2));
  const double n2m1 = nm1 * (2 + nm1);
  const double lambda = nm * 1e-9;
  const double king = (6 + 3 * 0.035) / (6 - 7 * 0.035);
  return 8 * std::pow(kPiD, 3) * n2m1 * n2m1 / (3 * 2.547e25 * std::pow(lambda, 4)) * king;
}

// Ozone absorption at the peak of its layer (per metre): the Chappuis band as a Gaussian (peak 4.6e-25 m^2 near
// 603 nm) times a peak number density of 5.4e18 per m^3 (300 Dobson units over the 10-40 km tent profile).
double ozoneBeta(double nm) { return 4.6e-25 * std::exp(-std::pow((nm - 603) / 80, 2)) * 5.4e18; }

double ozoneDensity(double h) {
  if (h < 10000 || h >= 40000) return 0;
  return h < 25000 ? (h - 10000) / 15000 : 1 - (h - 25000) / 15000;
}

// Spectral irradiance of the sun above the atmosphere (W / m^2 / nm): a 5778 K black body seen as 6.794e-5 sr.
double sunIrradiance(double nm) {
  const double h = 6.62607015e-34, c = 2.99792458e8, k = 1.380649e-23, T = 5778;
  const double l = nm * 1e-9;
  const double radiance = 2 * h * c * c / std::pow(l, 5) / std::expm1(h * c / (l * k * T));  // W / m^2 / sr / m
  return radiance * 6.794e-5 * 1e-9;
}

// CIE 1931 2-degree colour matching functions, the multi-lobe fit of Wyman, Sloan and Shirley (JCGT 2013).
double lobe(double x, double mu, double s1, double s2) {
  const double t = (x - mu) / (x < mu ? s1 : s2);
  return std::exp(-0.5 * t * t);
}
Vec3f spectrumToRgb(const std::array<double, kLambdas>& L) {
  double X = 0, Y = 0, Z = 0;
  for (int i = 0; i < kLambdas; ++i) {
    const double x = lambdaNm(i);
    X += L[i] * (1.056 * lobe(x, 599.8, 37.9, 31.0) + 0.362 * lobe(x, 442.0, 16.0, 26.7) -
                 0.065 * lobe(x, 501.1, 20.4, 26.2));
    Y += L[i] * (0.821 * lobe(x, 568.8, 46.9, 40.5) + 0.286 * lobe(x, 530.9, 16.3, 31.1));
    Z += L[i] * (1.217 * lobe(x, 437.0, 11.8, 36.0) + 0.681 * lobe(x, 459.0, 26.0, 13.8));
  }
  X *= 20;  // nm per sample
  Y *= 20;
  Z *= 20;
  // XYZ to linear Rec. 709 (D65).
  const double r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z;
  const double g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
  const double b = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
  return {static_cast<float>(std::max(0.0, r)), static_cast<float>(std::max(0.0, g)),
          static_cast<float>(std::max(0.0, b))};
}

struct V3 {
  double x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Distance along d (unit) from p to the sphere of radius r around the Earth's centre: the far hit (`far`) or the
// near hit in front of p; negative when there is none.
double sphereHit(V3 p, V3 d, double r, bool far) {
  const double b = dot(p, d), c = dot(p, p) - r * r;
  const double disc = b * b - c;
  if (disc < 0) return -1;
  const double s = std::sqrt(disc);
  return far ? -b + s : -b - s;
}

V3 direction(double elevation, double azimuth) {
  return {std::sin(azimuth) * std::cos(elevation), std::sin(elevation), -std::cos(azimuth) * std::cos(elevation)};
}

struct Atmosphere {
  std::array<double, kLambdas> rayleigh{}, ozone{}, sun{};
  double air = 1, dust = 1, ozoneAmount = 1;
  double altitude = 0;

  // Optical depth (Rayleigh, Mie, ozone density integrals) from p towards d up to the top; false if the Earth is
  // in the way.
  bool depthToSpace(V3 p, V3 d, V3& depth) const {
    if (sphereHit(p, d, kEarthRadius, false) > 0) return false;
    const double t = sphereHit(p, d, kAtmosphereRadius, true);
    depth = {0, 0, 0};
    for (int i = 0; i < kLightSteps; ++i) {
      double mid, ds;
      step(t, i, kLightSteps, mid, ds);
      const V3 q = p + d * mid;
      const double h = std::sqrt(dot(q, q)) - kEarthRadius;
      depth = depth + V3{std::exp(-h / kRayleighHeight), std::exp(-h / kMieHeight), ozoneDensity(h)} * ds;
    }
    return true;
  }

  std::array<double, kLambdas> transmittance(V3 depth) const {
    std::array<double, kLambdas> t{};
    for (int i = 0; i < kLambdas; ++i)
      t[i] = std::exp(-(rayleigh[i] * air * depth.x + kMieExtinction * dust * depth.y + ozone[i] * ozoneAmount * depth.z));
    return t;
  }

  // Single-scattered sky radiance (W / m^2 / sr / nm) seen along d with the sun towards s.
  std::array<double, kLambdas> radiance(V3 d, V3 s) const {
    const V3 p0{0, kEarthRadius + altitude, 0};
    double tEnd = sphereHit(p0, d, kAtmosphereRadius, true);
    const double ground = sphereHit(p0, d, kEarthRadius, false);
    if (ground > 0) tEnd = std::min(tEnd, ground);
    std::array<double, kLambdas> sumR{}, sumM{};
    V3 viewDepth{0, 0, 0};
    for (int i = 0; i < kViewSteps; ++i) {
      double mid, ds;
      step(tEnd, i, kViewSteps, mid, ds);
      const V3 p = p0 + d * mid;
      const double h = std::sqrt(dot(p, p)) - kEarthRadius;
      const V3 density{std::exp(-h / kRayleighHeight), std::exp(-h / kMieHeight), ozoneDensity(h)};
      const V3 half = density * (0.5 * ds);
      viewDepth = viewDepth + half;  // to the middle of this step
      V3 lightDepth;
      if (depthToSpace(p, s, lightDepth)) {
        const std::array<double, kLambdas> t = transmittance(viewDepth + lightDepth);
        for (int l = 0; l < kLambdas; ++l) {
          sumR[l] += density.x * t[l] * ds;
          sumM[l] += density.y * t[l] * ds;
        }
      }
      viewDepth = viewDepth + half;
    }
    const double mu = dot(d, s);
    const double phaseR = 3.0 / (16 * kPiD) * (1 + mu * mu);
    const double g2 = kMieG * kMieG;  // Cornette-Shanks
    const double phaseM =
        3 * (1 - g2) * (1 + mu * mu) / (8 * kPiD * (2 + g2) * std::pow(1 + g2 - 2 * kMieG * mu, 1.5));
    std::array<double, kLambdas> L{};
    for (int l = 0; l < kLambdas; ++l)
      L[l] = sun[l] * (rayleigh[l] * air * phaseR * sumR[l] + kMieScatter * dust * phaseM * sumM[l]);
    return L;
  }
};

// Runs f(i) for i in [0, n) on every usable core.
template <class F>
void parallelFor(int n, const F& f) {
  const int threads = static_cast<int>(std::max(1u, std::min<uint32_t>(usableCpuThreads(), 64)));
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t)
    pool.emplace_back([&, t] {
      for (int i = t; i < n; i += threads) f(i);
    });
  for (auto& th : pool) th.join();
}

}  // namespace

Vec3f sunDirection(const SkySettings& sky) {
  const V3 d = direction(sky.sun_elevation_deg * kPiD / 180, sky.sun_azimuth_deg * kPiD / 180);
  return {static_cast<float>(d.x), static_cast<float>(d.y), static_cast<float>(d.z)};
}

ImageRGB bakeSky(const SkySettings& sky) {
  Atmosphere atm;
  for (int i = 0; i < kLambdas; ++i) {
    atm.rayleigh[i] = rayleighBeta(lambdaNm(i));
    atm.ozone[i] = ozoneBeta(lambdaNm(i));
    atm.sun[i] = sunIrradiance(lambdaNm(i));
  }
  atm.air = sky.air;
  atm.dust = sky.dust;
  atm.ozoneAmount = sky.ozone;
  atm.altitude = std::clamp(sky.altitude, 1.0, kAtmosphereRadius - kEarthRadius - 1);  // >= 1 m: a downward ray finds the ground
  const double sunElevation = sky.sun_elevation_deg * kPiD / 180;
  const double sunAzimuth = sky.sun_azimuth_deg * kPiD / 180;
  const V3 s = direction(sunElevation, sunAzimuth);
  const double scale = kSkyScale * sky.strength;

  // The sky depends only on the view elevation and the azimuth from the sun: a table over both, denser near the
  // horizon (elevation = x |x| pi / 2), read with bilinear interpolation.
  constexpr int nE = 512, nA = 512;
  std::vector<Vec3f> table(size_t(nE) * nA);
  parallelFor(nE, [&](int ie) {
    const double x = 2.0 * ie / (nE - 1) - 1;
    const double e = x * std::fabs(x) * kPiD / 2;
    for (int ia = 0; ia < nA; ++ia) {
      const double da = kPiD * ia / (nA - 1);
      table[size_t(ie) * nA + ia] = spectrumToRgb(atm.radiance(direction(e, sunAzimuth + da), s)) * float(scale);
    }
  });

  // The sun's disc: its irradiance spread over its solid angle, dimmed by the air between it and the viewer.
  const double radius = sky.sun_size_deg * kPiD / 360;
  const double solidAngle = 2 * kPiD * (1 - std::cos(radius));
  Vec3f sunRgb(0);
  V3 sunDepth;
  if (sky.sun_disc && atm.depthToSpace({0, kEarthRadius + atm.altitude, 0}, s, sunDepth)) {
    const std::array<double, kLambdas> t = atm.transmittance(sunDepth);
    std::array<double, kLambdas> L{};
    for (int i = 0; i < kLambdas; ++i) L[i] = atm.sun[i] * t[i] / solidAngle;
    sunRgb = spectrumToRgb(L) * float(scale * sky.sun_strength);
  }

  ImageRGB map;
  map.width = std::max<uint32_t>(sky.resolution, 64);
  map.height = map.width / 2;
  map.pixels.assign(size_t(map.width) * map.height, Vec3f(0));
  const double cosSun = std::cos(radius);
  // Same layout as an environment map (core/render/lights.cpp uvToDirection, rotation 0).
  const auto pixelDirection = [&](double u, double v) {
    const double phi = (u - 0.5) * 2 * kPiD, theta = v * kPiD;
    return V3{-std::cos(phi) * std::sin(theta), std::cos(theta), -std::sin(phi) * std::sin(theta)};
  };
  const double pixelAngle = 2 * kPiD / map.width;
  parallelFor(static_cast<int>(map.height), [&](int y) {
    for (uint32_t x = 0; x < map.width; ++x) {
      const V3 d = pixelDirection((x + 0.5) / map.width, (y + 0.5) / map.height);
      const double e = std::asin(std::clamp(d.y, -1.0, 1.0));
      double da = std::fabs(std::atan2(d.x, -d.z) - sunAzimuth);
      da = std::fmod(da, 2 * kPiD);
      if (da > kPiD) da = 2 * kPiD - da;
      const double fe = (std::copysign(std::sqrt(std::fabs(e) / (kPiD / 2)), e) + 1) / 2 * (nE - 1);
      const double fa = da / kPiD * (nA - 1);
      const int e0 = std::min(static_cast<int>(fe), nE - 2), a0 = std::min(static_cast<int>(fa), nA - 2);
      const float te = static_cast<float>(fe - e0), ta = static_cast<float>(fa - a0);
      const auto at = [&](int ie, int ia) { return table[size_t(ie) * nA + ia]; };
      Vec3f c = (at(e0, a0) * (1 - ta) + at(e0, a0 + 1) * ta) * (1 - te) +
                (at(e0 + 1, a0) * (1 - ta) + at(e0 + 1, a0 + 1) * ta) * te;
      // Sun disc coverage, 8 x 8 subsamples per pixel near it, so its power does not depend on the resolution.
      // Limb darkening 1 - 0.6 (1 - cos) across the disc, normalized so the disc keeps the sun's whole irradiance
      // (Blender applies the same darkening without normalizing: its sun has 18% less power, D-120).
      if (!isBlack(sunRgb) && std::acos(std::clamp(dot(d, s), -1.0, 1.0)) < radius + 2 * pixelAngle) {
        double weight = 0;
        for (int sy = 0; sy < 8; ++sy)
          for (int sx = 0; sx < 8; ++sx) {
            const double cs = dot(pixelDirection((x + (sx + 0.5) / 8) / map.width, (y + (sy + 0.5) / 8) / map.height), s);
            if (cs <= cosSun) continue;
            const double f = std::acos(std::clamp(cs, -1.0, 1.0)) / radius;  // 0 at the centre, 1 at the edge
            weight += (1 - 0.6 * (1 - std::sqrt(std::max(0.0, 1 - f * f)))) / 0.8;
          }
        c += sunRgb * float(weight / 64);
      }
      map.at(x, static_cast<uint32_t>(y)) = c;
    }
  });
  return map;
}

}  // namespace pen
