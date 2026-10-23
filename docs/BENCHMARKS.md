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

## 2026-10-22, M7: multi-GPU worker (4x RTX 3060)

- **Machine:** vast.ai container, Ubuntu 24.04, 82 CPUs (cgroup v1 quota) of an AMD EPYC 7B12, 251 GB RAM,
  4x NVIDIA GeForce RTX 3060 12 GB (Ampere, sm_86), driver 580.65, CUDA 13.0, PCIe 2.0 x8 per GPU.
- **Build:** branch `m7` (3659f8f), Release, GCC 12.4.
- **Settings:** `cornell`, path integrator, 12 bounces. Rates from the render log.

| Run | Devices | Rate | Per GPU |
| --- | --- | --- | --- |
| 1080p, 256 spp | Vulkan, GPU 0 | 8.91 M samples/s | 8.9 |
| 1080p, 64 spp | CUDA, GPU 0 | 12.2 M samples/s | 12.2 |
| 4K, 2,048 spp (17 G samples, 483 s) | Vulkan, GPUs 0-3 | 35.2 M samples/s | 8.8 |

Four GPUs render 3.95x one GPU; items split 59 / 73 / 60 / 63 and every GPU stayed at 100% utilization.

The absolute rates are not representative of an RTX 3060. Both backends run at about 30-36 W of the 120 W limit at
"100% utilization", SM clock 1,447 MHz, with the driver reporting the GPU-idle clock reason. Vulkan RT is ~60x
slower than on the RTX 5070 Ti (M6) while CUDA is ~5x slower, so this host's GPUs look starved or capped. Moving the
shader binding table and frame constants to VRAM changed nothing (8.91 M/s before and after; not kept). To be
rechecked on other hardware in M9.
