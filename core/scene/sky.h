#pragma once

#include "core/image/image.h"
#include "core/math/vec.h"
#include "core/settings/render_settings.h"

namespace pen {

// The physical sky (M14, DECISIONS D-120): Nishita's single scattering by air (Rayleigh), aerosols (Mie) and
// ozone absorption over a spherical Earth, computed per wavelength (21 from 380 to 780 nm) and converted to linear
// Rec. 709 RGB, plus the sun's disc. Written from the physics and published data, not from Blender's code; its
// brightness is calibrated to Blender's Nishita sky (kSkyScale). The result is an equirectangular map for the
// environment (same layout as an HDRI), `resolution` x `resolution` / 2.
ImageRGB bakeSky(const SkySettings& sky);

// The unit vector towards the sun: elevation above the horizon, azimuth from -Z (glTF forward) towards +X.
Vec3f sunDirection(const SkySettings& sky);

}  // namespace pen
