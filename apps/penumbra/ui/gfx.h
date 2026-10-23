#pragma once

#include <memory>
#include <string>

struct GLFWwindow;

namespace pen::ui {

// Draws Dear ImGui's frames into the window: Vulkan, else OpenGL 3.3 for machines whose Vulkan cannot present
// (old drivers, virtual displays; docs/GUIDE.md "UI", DECISIONS D-100).
class Gfx {
 public:
  virtual ~Gfx() = default;
  virtual const char* name() const = 0;
  virtual void newFrame() = 0;
  // Renders ImGui's draw data over `clear` (RGBA, linear 0-1) and presents it.
  virtual void render(const float clear[4]) = 0;
  // Test only: saves the next rendered frame as a PNG (OpenGL; Vulkan returns false).
  virtual bool captureNextFrame(const std::string& pngPath) {
    (void)pngPath;
    return false;
  }
};

// Both take an ImGui context that exists already and set up ImGui's GLFW and renderer backends for `window`.
// They throw std::runtime_error with the reason when the API cannot drive this window.
std::unique_ptr<Gfx> createVulkanGfx(GLFWwindow* window);  // window created with GLFW_NO_API
std::unique_ptr<Gfx> createGlGfx(GLFWwindow* window);      // window with an OpenGL 3.3 core context

}  // namespace pen::ui
