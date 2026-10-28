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

## 2026-10-28, M9 (in progress): three single-GPU boxes in one datacenter, and the controller

- **Machines:** three vast.ai containers in one datacenter, Ubuntu 24.04, CUDA 13: RTX 4060 Ti 16 GB (controller,
  ~20 CPUs), RTX 5060 Ti 16 GB (worker, ~15 CPUs), RTX 4060 Ti 16 GB (worker, ~15 CPUs).
- **Build:** branch `m9` (cb9d132 / e705c94), Release, GCC 12.4. Path integrator, 12 bounces.

**Solo, 1080p, 20 s per run** (rate from the render log; GPU utilization from `nvidia-smi` every 0.5 s while it
renders):

| GPU | Scene | Vulkan RT | CUDA | Vulkan / CUDA | Util (Vulkan / CUDA) |
| --- | --- | --- | --- | --- | --- |
| RTX 5060 Ti | `cornell` | 279.6 M samples/s | 17.4 | 16x | 99% / 100% |
| RTX 5060 Ti | `sponza` (Khronos) | 60.6 | 6.8 | 8.9x | 97% / 100% |
| RTX 4060 Ti | `cornell` | 256.0-259.4 | 14.5-14.6 | 18x | 97-99% / 100% |
| RTX 4060 Ti | `sponza` (Khronos) | 40.2-40.4 | 3.8 | 10.6x | 98% / 100% |
| RTX 4060 Ti | `intel-sponza` (5.5 GB in VRAM) | 11.8 | 1.6 | 7.2x | 78% / 99% |

The two RTX 4060 Ti agree within 1%. CUDA is 7-18x slower than Vulkan RT while reporting 100% busy: the GPU is
occupied but the kernel is inefficient (M9 step 4). Vulkan on the heavy Intel Sponza leaves the GPU 22% idle (step 3).

**Three machines, one render** (controller hosts and renders; workers join over the datacenter network):

| Scene | Result | Per GPU (items) | GPU util while working |
| --- | --- | --- | --- |
| `sponza`, 4K, 1,024 spp | 66.8 s, 127.1 M samples/s = 90% of the three solo rates added (141.2) | 4060 Ti (ctl) 42, 4060 Ti 40, 5060 Ti 53 | 100% / 94% / 95% |
| `intel-sponza`, 4K, 256 spp, cached on the workers | 59.3 s, 35.8 M samples/s | 81 / 21 / 33 (the workers load the scene for ~45 s first) | 92% / 78% / 81% |

The 58 MB Khronos Sponza package reached each worker in 0.6 s; the 2.2 GB Intel Sponza package in 35-36 s
(~60 MB/s, both workers at once). With realistic scenes the controller is nearly idle (network threads 1%,
2 results/s).

**Controller throughput, `net.scale_64`** (laptop, i7-11850H; 64 workers with 8 null devices each, 4K, 4,096 spp):

| Build | Time | Results/s | Samples/s | Network threads |
| --- | --- | --- | --- | --- |
| before M9 | 95-113 s | 104 | 300 M | one thread, 96-98% busy |
| tail search once per hand-out call | 48 s | 214 | 700 M | 96% (decrypting 21 s) |
| + several network threads (strands) | 19.4 s | 442 | 1,748 M | 332% of one core over 4 threads |
