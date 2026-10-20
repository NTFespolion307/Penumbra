#pragma once

#include <string>
#include <vector>

#include "core/math/transform.h"
#include "core/scene/scene.h"
#include "core/settings/render_settings.h"

namespace pen {

// The camera a render uses: the scene camera picked by --camera (or the first one, or an automatic camera
// framing the scene), with .pen.json overrides and the image aspect applied.
class RenderCamera {
 public:
  RenderCamera() = default;
  // Throws SceneError if --camera names a camera the scene does not have. `notes` receives info lines.
  RenderCamera(const Scene& scene, const RenderSettings& settings, uint32_t width, uint32_t height,
               std::vector<std::string>& notes);

  // Ray through image position (px, py) in pixels (0..width, 0..height, y down); lens in [0,1)^2.
  void generateRay(float px, float py, Vec2f lens, Vec3f& origin, Vec3f& dir) const;

  const Camera& camera() const { return camera_; }
  // The values generateRay uses, for GPU backends that run their own copy of it.
  float width() const { return width_; }
  float height() const { return height_; }
  float tanX() const { return tan_x_; }
  float tanY() const { return tan_y_; }

 private:
  Camera camera_;
  float width_ = 1, height_ = 1;
  float tan_x_ = 1, tan_y_ = 1;
};

}  // namespace pen
