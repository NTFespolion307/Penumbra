// The CUDA path tracer: integrator.slang with rays traced in software (bvh.slang). slangc turns this file into
// CUDA C++, nvcc into a fatbin (DECISIONS D-085, D-087). One thread per pixel of the launch.
#include "scene.slang"
#include "bvh.slang"
#include "integrator.slang"

[shader("compute")]
[numthreads(16, 8, 1)]
void render(uint3 id: SV_DispatchThreadID, uniform PushConstants item) { renderPixel(id.xy, item); }
