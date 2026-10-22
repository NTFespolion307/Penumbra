# Penumbra benchmarks

Measured speeds, newest entry last (docs/GUIDE.md "Benchmarks"). Samples/s counts camera samples (full paths)
per second. Every entry names the machine, the GPU and the commit.

## 2026-10-20, M5: first Vulkan RT numbers

- **Machine:** vast.ai container, Ubuntu 24.04, 48 rented cores of an Intel Xeon E7-8890 v4 @ 2.2 GHz (Broadwell),
  128 GB RAM, NVIDIA GeForce RTX 5060 Ti 16 GB (Blackwell), driver 590.48.01.
- **Build:** branch `m5`, Release, GCC 12.4. Megakernel ray tracing pipeline (DECISIONS D-078).
- **Settings:** 1080p, path integrator, 12 bounces, test scenes from `penumbra-scenegen`. The GPU rendered 1,024
  spp, the CPU 64 spp with `--threads 48`; both are single-device runs, so the rate covers everything after scene
  load (film merge and output included).

| Scene | GPU: Vulkan RT | CPU: Embree, 48 threads | GPU / CPU |
| --- | --- | --- | --- |
| `cornell` (Lambert box, area light) | 245 M samples/s | 15.5 M samples/s | 15.8x |
| `spheres` (glass, gold, clear coat, sheen on an HDRI) | 234 M samples/s | 15.0 M samples/s | 15.6x |
| `bigtex` (one textured quad, uniform light) | 598 M samples/s | 42.6 M samples/s | 14.0x |

Same images: at 4,096 spp the Vulkan render matches the CPU golden Cornell image to relMSE 1.1e-8 (the gate
allows 1e-3), because both draw the same random samples and differ only by float rounding.

Not measured yet: rays/s per bounce, scene upload and BVH build times on large scenes, network throughput (M9).

## 2026-10-22, M6: CUDA compute backend (software BVH)

- **Machine:** vast.ai container, Ubuntu 24.04, 30 CPUs (cgroup quota) of an AMD Ryzen 9 5900XT, 62 GB RAM,
  NVIDIA GeForce RTX 5070 Ti 16 GB (Blackwell, sm_120), driver 610.57, CUDA 13.3.
- **Build:** branch `m6-wip` (4e01846), Release, GCC 12.4; CUDA kernels for sm_75 to sm_120 from one compute_75 PTX.
- **Settings:** 1080p, path integrator, 12 bounces, test scenes from `penumbra-scenegen`; GPU 1,024 spp, CPU 64 spp.
  Rates are the device's own time from the render log (after scene upload), one device per run.

| Scene | Vulkan RT | CUDA (software BVH) | CPU: Embree, 31 threads | Vulkan / CUDA |
| --- | --- | --- | --- | --- |
| `cornell` | 532 M samples/s | 58.3 M samples/s | 14.4 M samples/s | 9.1x |
| `spheres` | 471 M samples/s | 48.7 M samples/s | 16.5 M samples/s | 9.7x |
| `bigtex` (one quad) | 1,726 M samples/s | 2,308 M samples/s | 46.6 M samples/s | 0.75x |
| `cutout` (alpha, instances, orthographic) | 553 M samples/s | 67.9 M samples/s | 19.8 M samples/s | 8.1x |

On a GPU with RT cores the CUDA path is about 9x slower than Vulkan RT, not the guide's 2 to 5x. Shading is not the
cost: on `bigtex`, which has nothing to traverse, CUDA is faster than Vulkan. The megakernel uses 173 registers per
thread and an 840-byte stack (two traversal stacks), which limits occupancy. M9 tunes it: register caps, megakernel
vs wavefront, and stack size. CUDA is only the default on GPUs without ray tracing, where Vulkan RT is not an option.
