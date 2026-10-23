#include "apps/penumbra/ui/gfx.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <stdexcept>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

// ImGui's OpenGL backend loads the GL functions itself (libGL / opengl32 at run time); these are the only GL calls
// Penumbra makes, so they come from the same loader.
#include <imgui_impl_opengl3_loader.h>

namespace pen::ui {

namespace {

class GlGfx final : public Gfx {
 public:
  explicit GlGfx(GLFWwindow* window) : window_(window) {
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);
    if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) throw std::runtime_error("ImGui GLFW backend failed");
    if (!ImGui_ImplOpenGL3_Init("#version 330")) {
      ImGui_ImplGlfw_Shutdown();
      throw std::runtime_error("OpenGL 3.3 is not available (ImGui OpenGL backend failed)");
    }
  }
  ~GlGfx() override {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
  }
  const char* name() const override { return "OpenGL 3.3"; }
  bool captureNextFrame(const std::string& pngPath) override {
    capture_ = pngPath;
    return true;
  }
  void newFrame() override {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
  }
  void render(const float clear[4]) override {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (!capture_.empty()) {  // the back buffer, before it is shown; rows bottom-up
      std::vector<unsigned char> px(size_t(w) * h * 4);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      stbi_flip_vertically_on_write(1);
      stbi_write_png(capture_.c_str(), w, h, 4, px.data(), w * 4);
      capture_.clear();
    }
    glfwSwapBuffers(window_);
  }

 private:
  GLFWwindow* window_;
  std::string capture_;
};

}  // namespace

std::unique_ptr<Gfx> createGlGfx(GLFWwindow* window) { return std::make_unique<GlGfx>(window); }

}  // namespace pen::ui
