#include "core/render/camera.h"

#include <fmt/format.h>

#include "core/sampling/warp.h"
#include "core/scene/scene_loader.h"

namespace pen {

namespace {

// Looks at the scene centre from +Z (glTF front), far enough to fit the bounding sphere.
Camera autoCamera(const Scene& scene) {
  Camera c;
  c.name = "automatic";
  c.yfov = 39.6f * kPi / 180.0f;
  const Vec3f center = scene.bounds.center();
  const float r = scene.boundingRadius();
  const float dist = r / std::sin(c.yfov / 2) * 1.05f;
  c.to_world = Affine3f::translate(center + Vec3f(0, 0, dist));
  return c;
}

}  // namespace

RenderCamera::RenderCamera(const Scene& scene, const RenderSettings& settings, uint32_t width, uint32_t height,
                           std::vector<std::string>& notes)
    : width_(static_cast<float>(width)), height_(static_cast<float>(height)) {
  if (!settings.camera.empty()) {
    bool found = false;
    for (const Camera& c : scene.cameras) {
      if (c.name == settings.camera) {
        camera_ = c;
        found = true;
        break;
      }
    }
    if (!found) {
      std::string names;
      for (const Camera& c : scene.cameras) names += (names.empty() ? "" : ", ") + c.name;
      throw SceneError(fmt::format("camera '{}' is not in the scene (cameras: {})", settings.camera,
                                   names.empty() ? "none" : names));
    }
  } else if (!scene.cameras.empty()) {
    camera_ = scene.cameras.front();
  } else {
    camera_ = autoCamera(scene);
    notes.push_back("the scene has no camera; using an automatic camera that frames the whole scene");
  }

  const CameraOverrides& co = settings.camera_overrides;
  if (co.projection == "perspective") camera_.projection = CameraProjection::Perspective;
  if (co.projection == "orthographic") camera_.projection = CameraProjection::Orthographic;
  if (co.projection == "equirectangular") camera_.projection = CameraProjection::Equirectangular;

  const float aspect = width_ / height_;
  tan_y_ = std::tan(camera_.yfov / 2);
  tan_x_ = tan_y_ * aspect;

  if (co.fstop > 0 && camera_.projection == CameraProjection::Perspective) {
    // Focal length from the horizontal field of view and the sensor width; scene units are metres.
    const float focalMm = static_cast<float>(co.sensor_width_mm) / 2 / tan_x_;
    camera_.aperture_radius = focalMm / 1000.0f / (2 * static_cast<float>(co.fstop));
    if (co.focus_distance > 0) {
      camera_.focus_distance = static_cast<float>(co.focus_distance);
    } else {
      const Vec3f eye = camera_.to_world.point(Vec3f(0));
      const Vec3f fwd = normalize(camera_.to_world.vector(Vec3f(0, 0, -1)));
      camera_.focus_distance = std::fmax(dot(scene.bounds.center() - eye, fwd), 1e-3f);
    }
    notes.push_back(fmt::format("depth of field: f/{} -> aperture radius {:.4g}, focus distance {:.4g}", co.fstop,
                                camera_.aperture_radius, camera_.focus_distance));
  }
  notes.push_back(fmt::format("camera '{}' ({})", camera_.name,
                              camera_.projection == CameraProjection::Perspective    ? "perspective"
                              : camera_.projection == CameraProjection::Orthographic ? "orthographic"
                                                                                     : "equirectangular"));
}

void RenderCamera::generateRay(float px, float py, Vec2f lens, Vec3f& origin, Vec3f& dir) const {
  const float sx = 2 * px / width_ - 1;  // -1 left .. +1 right
  const float sy = 1 - 2 * py / height_; // +1 top .. -1 bottom
  Vec3f o(0), d;
  switch (camera_.projection) {
    case CameraProjection::Orthographic: {
      const float aspect = width_ / height_;
      o = {sx * camera_.ymag * aspect, sy * camera_.ymag, 0};
      d = {0, 0, -1};
      break;
    }
    case CameraProjection::Equirectangular: {
      const float phi = (px / width_ - 0.5f) * kTwoPi;
      const float theta = py / height_ * kPi;
      d = {std::sin(phi) * std::sin(theta), std::cos(theta), -std::cos(phi) * std::sin(theta)};
      break;
    }
    case CameraProjection::Perspective:
    default: {
      d = normalize(Vec3f(sx * tan_x_, sy * tan_y_, -1));
      if (camera_.aperture_radius > 0) {
        const Vec3f focus = d * (camera_.focus_distance / -d.z);
        const Vec2f disk = sampleConcentricDisk(lens);
        o = {disk.x * camera_.aperture_radius, disk.y * camera_.aperture_radius, 0};
        d = normalize(focus - o);
      }
      break;
    }
  }
  origin = camera_.to_world.point(o);
  dir = normalize(camera_.to_world.vector(d));
}

}  // namespace pen
