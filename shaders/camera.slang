// Port of RenderCamera::generateRay (core/render/camera.cpp). The camera itself (choice, overrides, depth of
// field) is set up on the CPU and passed in FrameData.
#pragma once
#include "scene.slang"
#include "warp.slang"

void generateRay(float px, float py, float2 lens, out float3 origin, out float3 dir) {
  FrameData f = g_frame;
  float sx = 2 * px / f.width - 1;
  float sy = 1 - 2 * py / f.height;
  float3 o = float3(0, 0, 0), d;
  if (f.projection == 1) {
    float aspect = f.width / f.height;
    o = float3(sx * f.ymag * aspect, sy * f.ymag, 0);
    d = float3(0, 0, -1);
  } else if (f.projection == 2) {
    float phi = (px / f.width - 0.5) * kTwoPi;
    float theta = py / f.height * kPi;
    d = float3(sin(phi) * sin(theta), cos(theta), -cos(phi) * sin(theta));
  } else {
    d = normalizeSafe(float3(sx * f.tan_x, sy * f.tan_y, -1));
    if (f.aperture_radius > 0) {
      float3 focus = d * (f.focus_distance / -d.z);
      float2 disk = sampleConcentricDisk(lens);
      o = float3(disk.x * f.aperture_radius, disk.y * f.aperture_radius, 0);
      d = normalizeSafe(focus - o);
    }
  }
  origin = xformPoint(f.cam_to_world, o);
  dir = normalizeSafe(xformVector(f.cam_to_world, d));
}
