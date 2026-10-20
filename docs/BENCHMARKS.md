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
