#include "apps/penumbra/ui/texture.h"

#include <imgui_internal.h>  // RegisterUserTexture (ImGui 1.92, documented as experimental)

#include <algorithm>
#include <cstring>

namespace pen::ui {

namespace {

// Textures asked to be destroyed: kept until the renderer backend has destroyed its copy.
std::vector<ImTextureData*>& graveyard() {
  static std::vector<ImTextureData*> g;
  return g;
}

void retire(ImTextureData* t) {
  if (!t) return;
  if (t->Status == ImTextureStatus_OK || t->Status == ImTextureStatus_WantUpdates) t->SetStatus(ImTextureStatus_WantDestroy);
  graveyard().push_back(t);
}

}  // namespace

ImageTexture::~ImageTexture() { retire(tex_); }

void ImageTexture::set(const uint8_t* rgba, int width, int height) {
  if (!tex_ || tex_->Width != width || tex_->Height != height) {
    retire(tex_);
    tex_ = IM_NEW(ImTextureData)();
    tex_->Create(ImTextureFormat_RGBA32, width, height);
    std::memcpy(tex_->GetPixels(), rgba, size_t(width) * height * 4);
    tex_->UseColors = true;
    ImGui::RegisterUserTexture(tex_);  // status WantCreate: the backend uploads it next frame
    return;
  }
  std::memcpy(tex_->GetPixels(), rgba, size_t(width) * height * 4);
  if (tex_->Status == ImTextureStatus_OK || tex_->Status == ImTextureStatus_WantUpdates) {
    ImTextureRect all{0, 0, static_cast<unsigned short>(width), static_cast<unsigned short>(height)};
    tex_->Updates.clear();
    tex_->Updates.push_back(all);
    tex_->UpdateRect = all;
    tex_->SetStatus(ImTextureStatus_WantUpdates);
  }  // still WantCreate: the pending upload takes the new pixels
}

void ImageTexture::collect() {
  auto& g = graveyard();
  for (auto it = g.begin(); it != g.end();) {
    ImTextureData* t = *it;
    if (t->Status == ImTextureStatus_Destroyed || t->Status == ImTextureStatus_WantCreate) {
      // Destroyed by the backend, or never uploaded: nothing on the GPU any more.
      ImGui::UnregisterUserTexture(t);
      IM_DELETE(t);
      it = g.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace pen::ui
