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

Correction (D-114): these rates are samples over the whole `--time-limit`, and that clock also counts the scene
upload and build on the device. With an 8 s build in a 20 s run, Intel Sponza's 11.8 understates Vulkan's speed, and
its 78% averaged upload samples into the utilization. `penumbra --benchmark` now measures over each device's render
window; rerun these rows with it.

**Three machines, one render** (controller hosts and renders; workers join over the datacenter network):

| Scene | Result | Per GPU (items) | GPU util while working |
| --- | --- | --- | --- |
| `sponza`, 4K, 1,024 spp | 66.8 s, 127.1 M samples/s = 90% of the three solo rates added (141.2) | 4060 Ti (ctl) 42, 4060 Ti 40, 5060 Ti 53 | 100% / 94% / 95% |
| `intel-sponza`, 4K, 256 spp, cached on the workers | 59.3 s, 35.8 M samples/s | 81 / 21 / 33 (the workers load the scene for ~45 s first) | 92% / 78% / 81% |

The 58 MB Khronos Sponza package reached each worker in 0.6 s; the 2.2 GB Intel Sponza package reached both
workers at once in 35-36 s: ~117 MB/s in total, the line rate of the controller's 1 Gbit/s Ethernet (the user
confirmed it was on 1 GbE, not its 10 GbE port). The transfer filled the link; it is not a Penumbra limit. With realistic scenes the controller is nearly idle (network threads 1%,
2 results/s).

**Controller throughput, `net.scale_64`** (laptop, i7-11850H; 64 workers with 8 null devices each, 4K, 4,096 spp):

| Build | Time | Results/s | Samples/s | Network threads |
| --- | --- | --- | --- | --- |
| before M9 | 95-113 s | 104 | 300 M | one thread, 96-98% busy |
| tail search once per hand-out call | 48 s | 214 | 700 M | 96% (decrypting 21 s) |
| + several network threads (strands) | 19.4 s | 442 | 1,748 M | 332% of one core over 4 threads |

## 2026-10-29, M9: `penumbra --benchmark` on three Blackwell boxes

- **Machines (vast.ai, Ubuntu 24.04, CUDA 13, sm_120):** RTX 5060 Ti 16 GB (driver 595.80, 14 CPUs), RTX 5070 Ti
  16 GB (driver 580.119, ~15 CPUs), RTX PRO 6000 Blackwell Server Edition 96 GB (driver 580.95; the user's stand-in
  for a 5090).
- **Build:** branch `m9` b8590ac, Release, GCC 12.4. `penumbra --benchmark --devices 0:vulkan,0:cuda`: 1080p, 30 s
  per run, path integrator, 12 bounces. Rates count only the device's render window (D-114).

| GPU | Scene | Vulkan RT (M samples/s) | CUDA | Vulkan / CUDA | Busy % (both) | Upload + build (Vulkan / CUDA) |
| --- | --- | --- | --- | --- | --- | --- |
| RTX 5060 Ti | `cornell` | 290.3 | 19.0 | 15x | 100 | 0.1 s / 0.0 s |
| RTX 5060 Ti | `spheres` | 268.7 | 19.5 | 14x | 100 | 0.0 s / 0.0 s |
| RTX 5060 Ti | `sponza` (Khronos) | 60.9 | 6.9 | 8.8x | 100 | 0.3 s / 0.4 s |
| RTX 5060 Ti | `intel-sponza` (5.5 GB) | 42.8 | 2.3 | 19x | 100 | 3.9 s / 5.1 s |
| RTX 5070 Ti | `cornell` | 388.5 | 43.2 | 9.0x | 100 | 0.2 s / 0.0 s |
| RTX 5070 Ti | `spheres` | 345.8 | 54.4 | 6.4x | 100 | 0.0 s / 0.1 s |
| RTX 5070 Ti | `sponza` (Khronos) | 87.8 | 15.0 | 5.9x | 100 | 0.8 s / 0.9 s |
| RTX 5070 Ti | `intel-sponza` (5.5 GB) | 65.8 | 4.8 | 14x | 100 | 10.1 s / 12.8 s |
| RTX PRO 6000 | `cornell` | 478.8 | 90.4 | 5.3x | 99 / 100 | 0.1 s / 0.0 s |
| RTX PRO 6000 | `spheres` | 403.8 | 66.5 | 6.1x | 100 | 0.0 s / 0.1 s |
| RTX PRO 6000 | `sponza` (Khronos) | 105.9 | 27.5 | 3.8x | 100 | 0.8 s / 0.7 s |
| RTX PRO 6000 | `intel-sponza` (5.5 GB) | 79.4 | 8.7 | 9.1x | 100 | 11.9 s / 13.7 s |

Every device is 99-100% busy over its render window: the GPU does not wait between work items, also not on the
5.5 GB Intel Sponza (the 78% of the 2026-10-28 entry counted the upload). Vulkan RT is 4-19x faster than the CUDA
software BVH (the guide estimated 2-5x). Traversal alone, Sponza on the RTX 5070 Ti: `debug:normal` (one primary ray
per sample) 1,399 vs 200 M samples/s, `ao` 748 vs 86 (D-115).

**Three machines over the internet, one render** (PRO 6000 hosts and renders; the 5070 Ti and 5060 Ti join from two
other datacenters with `--stay`, scene already cached; Vulkan; 4K):

| Scene | Result | Per GPU: items, rendering time | Busy while joined |
| --- | --- | --- | --- |
| `sponza`, 2048 spp | 58.6 s, 289.7 M samples/s | PRO 6000 145 items, 57.3 s; 5070 Ti 70, 40.0 s; 5060 Ti 40, 40.9 s | 98% / ~98% / ~99% |
| `intel-sponza`, 512 spp | 48.7 s, 87.2 M samples/s | PRO 6000 alone (135 items, 36.1 s) | 93% (nvidia-smi) |

The workers joined 16 s after the render started: after the previous render's controller exited they retry with
backoff up to 30 s (D-113). From their first item (14:56:21) to their last result (14:57:02) they rendered 40-41 s
of 41 s. On Intel Sponza the PRO 6000 finished in 36 s, before a worker had received (2.1 GB in 22 s to the
5070 Ti) and loaded the scene. Controller: 4 results/s, unpacking + merging < 1% of a core, network threads 2%.
Raw TCP from the controller: 180 MB/s to the 5070 Ti, 62 MB/s to the 5060 Ti.

## 2026-10-29, M14: the atmosphere and the Swiss Alps at 8K (RTX 5090)

- **Machine:** rented RTX 5090 32 GB (driver 595.71, Ubuntu 24.04, 23-CPU quota). Branch `m14` 7f77d21, Release.
- **Scene:** `scenes/swiss-alps.pen.json`: 32.0 M triangles (4 m grid over 16 x 16 km), an 8000 x 8000 photo texture,
  the physical sky (sun 22 degrees, azimuth 300) and the atmosphere (air, haze, valley fog).

| Render | Time | M samples/s | VRAM |
| --- | --- | --- | --- |
| 8K (7680 x 4320), 1024 spp, Vulkan RT | 78.4 s (render 76.6 s) | 433 | 3.6 GB (tier 0) |

The sky bake adds about 1.5 s to every scene load (laptop).

