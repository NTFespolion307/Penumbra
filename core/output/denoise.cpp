#include "core/output/denoise.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#if defined(PEN_HAS_OIDN)
#include <OpenImageDenoise/oidn.hpp>
#include <fmt/format.h>
#endif

namespace pen {

float autoExposure(const std::vector<Vec3f>& pixels) {
  double sum = 0;
  size_t n = 0;
  for (const Vec3f& p : pixels) {
    const float l = luminance(p);
    if (l > 1e-8f && std::isfinite(l)) {
      sum += std::log(double(l));
      ++n;
    }
  }
  return n ? static_cast<float>(0.18 / std::exp(sum / double(n))) : 1.0f;
}

#if defined(PEN_HAS_OIDN)

struct OidnDenoiser::Impl {
  oidn::DeviceRef device;
  std::string name;
  uint32_t alignment = 1, overlap = 0;
};

OidnDenoiser::OidnDenoiser() : impl_(std::make_unique<Impl>()) {
  impl_->device = oidn::newDevice();  // the best device OIDN finds: CUDA on an NVIDIA GPU, else the CPU
  impl_->device.commit();
  const char* error = nullptr;
  if (impl_->device.getError(error) != oidn::Error::None)
    throw std::runtime_error(fmt::format("Open Image Denoise: {}", error ? error : "cannot create a device"));
  const auto type = impl_->device.get<oidn::DeviceType>("type");
  impl_->name = type == oidn::DeviceType::CUDA ? "CUDA (NVIDIA GPU)"
                : type == oidn::DeviceType::CPU ? "CPU"
                                                : "another device";
  // The tiling constants are known once a filter is committed with images (before that they read 0).
  constexpr uint32_t kProbe = 64;
  const size_t bytes = size_t(kProbe) * kProbe * sizeof(Vec3f);
  oidn::BufferRef probe = impl_->device.newBuffer(bytes), out = impl_->device.newBuffer(bytes);
  oidn::FilterRef f = impl_->device.newFilter("RT");
  f.setImage("color", probe, oidn::Format::Float3, kProbe, kProbe);
  f.setImage("output", out, oidn::Format::Float3, kProbe, kProbe);
  f.set("hdr", true);
  f.commit();
  impl_->alignment = static_cast<uint32_t>(std::max(1, f.get<int>("tileAlignment")));
  impl_->overlap = static_cast<uint32_t>(std::max(0, f.get<int>("tileOverlap")));
}

uint32_t OidnDenoiser::tileAlignment() const { return impl_->alignment; }
uint32_t OidnDenoiser::tileOverlap() const { return impl_->overlap; }

OidnDenoiser::~OidnDenoiser() = default;

std::string OidnDenoiser::describe() const { return "Open Image Denoise " + std::to_string(OIDN_VERSION_MAJOR) + "." +
                                                std::to_string(OIDN_VERSION_MINOR) + " on " + impl_->name; }

void OidnDenoiser::run(std::vector<Vec3f>& color, const std::vector<Vec3f>& albedo, const std::vector<Vec3f>& normal,
                   uint32_t w, uint32_t h, float inputScale) {
  const size_t bytes = size_t(w) * h * sizeof(Vec3f);
  oidn::BufferRef c = impl_->device.newBuffer(bytes), a = impl_->device.newBuffer(bytes), n = impl_->device.newBuffer(bytes);
  c.write(0, bytes, color.data());
  a.write(0, bytes, albedo.data());
  n.write(0, bytes, normal.data());
  oidn::FilterRef f = impl_->device.newFilter("RT");
  f.setImage("color", c, oidn::Format::Float3, w, h);
  f.setImage("albedo", a, oidn::Format::Float3, w, h);
  f.setImage("normal", n, oidn::Format::Float3, w, h);
  f.setImage("output", c, oidn::Format::Float3, w, h);
  f.set("hdr", true);
  f.set("inputScale", inputScale);
  f.commit();
  f.execute();
  const char* error = nullptr;
  if (impl_->device.getError(error) != oidn::Error::None)
    throw std::runtime_error(fmt::format("Open Image Denoise: {}", error ? error : "the filter failed"));
  c.read(0, bytes, color.data());
}

bool denoiserAvailable() { return true; }

#else

struct OidnDenoiser::Impl {};
OidnDenoiser::OidnDenoiser() { throw std::runtime_error("this build has no Open Image Denoise (PEN_ENABLE_OIDN=OFF)"); }
OidnDenoiser::~OidnDenoiser() = default;
std::string OidnDenoiser::describe() const { return {}; }
uint32_t OidnDenoiser::tileAlignment() const { return 1; }
uint32_t OidnDenoiser::tileOverlap() const { return 0; }
void OidnDenoiser::run(std::vector<Vec3f>&, const std::vector<Vec3f>&, const std::vector<Vec3f>&, uint32_t, uint32_t,
                       float) {}
bool denoiserAvailable() { return false; }

#endif

}  // namespace pen
