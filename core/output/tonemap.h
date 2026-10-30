#pragma once

#include "core/math/vec.h"
#include "core/settings/render_settings.h"

namespace pen {

// The display transform (M11, DECISIONS D-126): scene-linear Rec. 709 radiance, already scaled by the exposure,
// to display-encoded sRGB in [0, 1]. AgX follows Blender 4.x (Rec. 2020 inset and outset matrices, log2 encoding
// from -12.47 to +4.03 EV around 0.18, its sigmoid's polynomial fit); Filmic follows Blender's Filmic (Medium
// Contrast) grey response; ACES is Stephen Hill's fit of the RRT and sRGB ODT; None is the sRGB curve with
// clipping.
Vec3f tonemap(Tonemap mode, Vec3f linear);

}  // namespace pen
