#pragma once

#include <imgui.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace pen::ui {

// An RGBA8 image shown with ImGui::Image (framing preview, live viewport). ImGui's renderer backends upload it
// (ImTextureData, registered as a user texture), so it works the same with Vulkan and OpenGL.
class ImageTexture {
 public:
  ImageTexture() = default;
  ~ImageTexture();
  ImageTexture(const ImageTexture&) = delete;
  ImageTexture& operator=(const ImageTexture&) = delete;

  // Replaces the pixels (`rgba` is width x height x 4 bytes, rows top-down). The upload happens with the next
  // rendered frame.
  void set(const uint8_t* rgba, int width, int height);
  bool valid() const { return tex_ != nullptr; }
  int width() const { return tex_ ? tex_->Width : 0; }
  int height() const { return tex_ ? tex_->Height : 0; }
  ImTextureRef ref() const { return tex_->GetTexRef(); }  // not ImTextureRef(tex_): that is the legacy void* ID

  // Called once per frame: frees textures the renderer has finished destroying.
  static void collect();

 private:
  ImTextureData* tex_ = nullptr;
};

}  // namespace pen::ui
