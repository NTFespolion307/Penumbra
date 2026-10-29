# Penumbra

Penumbra is a distributed, multi-GPU path tracer for **Windows 10/11, Ubuntu 22.04+ and Debian 12+**. It renders
still images from 120p up to 160K (163840 × 92160, 15.1 gigapixels) from glTF scenes exported from Blender.

One machine is the **controller**: it hosts the render, sends the scene out and collects the finished tiles into
the image. Any number of **workers**, each with one or more GPUs, join with the controller's address and a join
code, render tiles and send them back. A render on one machine is the same thing with no workers, and every
combination of machines, GPUs and backends converges to the same image.

> **Status: milestone M9 (performance and benchmarks; M14 comes next).** Penumbra renders glTF and OBJ scenes on GPUs with hardware ray
> tracing (Vulkan RT), on any NVIDIA GPU through CUDA, and on the CPU (Embree), with a path tracer (next-event
> estimation, MIS), the glTF PBR material with its common extensions and HDRI lighting, at any size up to 160K,
> with checkpoints, `--resume` and `merge`, writing EXR, PNG, JPEG, TIFF or WebP. Every machine uses all its GPUs
> at once, with Vulkan, CUDA and the CPU mixed freely; work is split into tile and sample chunks that the devices
> share, idle devices take over the last chunks of slower ones, and a GPU that fails, hangs or cannot hold the
> scene is dropped without changing the image. Scenes bigger than a GPU's memory still render: textures move to RAM
> or are downsized, and a GPU that cannot hold the geometry sits the render out; running out of memory never
> crashes a render. Several machines render together: a controller hosts, workers on
> the LAN or the internet join with a code (encrypted), download the scene once, and any failure of a worker, its
> devices or the network is recovered without changing the image. The UI hosts, joins and runs renders with
> clicks only: Dashboard, Hosts, Render, a live Viewport and Settings, on Windows and Linux. `penumbra --benchmark`
> measures every device (measured numbers: [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md)); workers with a slow uplink
> get larger items. An atmosphere and physical sky arrive in M14. Features marked *(planned, Mx)* below are not built yet. The roadmap is in
> [`docs/GUIDE.md`](docs/GUIDE.md).


**Supported GPUs:** NVIDIA first: RTX 20 series and newer through Vulkan ray tracing (tested on an RTX 5060 Ti
under Linux), and any NVIDIA GPU from compute capability 7.5 (Turing) through CUDA with a software BVH (tested on an RTX 5070
Ti and a GTX 1660 SUPER). The NVIDIA driver also offers Vulkan ray tracing on GTX 16-series cards (emulated on the
shader cores), so those use Vulkan by default. Older NVIDIA GPUs need a build with CUDA 12 (see Build options). AMD RDNA2+ and Intel Arc should work through Vulkan
ray tracing but are untested. Integrated Intel/AMD GPUs without ray tracing do not render in v1; the CPU renders instead.
On the GPU, an image matches the CPU's within float rounding (relMSE about 1e-8 at 4,096 samples per pixel).

## Contents

1. [Why you build it yourself](#why-you-build-it-yourself)
2. [Prerequisites](#prerequisites)
3. [Build](#build)
4. [Running on one machine](#running-on-one-machine)
5. [Rendering with several machines](#rendering-with-several-machines)
6. [CLI reference](#cli-reference)
7. [Supported formats](#supported-formats)
8. [Resolutions, backends, memory and formats](#resolutions-backends-memory-and-formats)
9. [Scenes from Blender](#scenes-from-blender)
10. [Troubleshooting](#troubleshooting)
11. [Updating](#updating)

## Why you build it yourself

Windows SmartScreen blocks unsigned `.exe` files downloaded from the internet, and Penumbra is not signed. An
exe you build on your own machine carries no download mark and runs without a warning, so building from source
is the supported way to install Penumbra on every machine, Windows or Linux.

## Prerequisites

Every machine that runs Penumbra (controller and workers) needs these. Versions are minimums.

### Windows 10/11 (x64)

| Tool | Version | Notes |
| --- | --- | --- |
| Visual Studio 2022 (17.10+) or 2026, or the Build Tools | MSVC 19.40+ | Workload "Desktop development with C++" (includes CMake and Ninja) |
| Git | any recent | |
| CMake | 3.26+ | Bundled with Visual Studio; a separate install also works |
| vcpkg | current | Package manager for the C++ libraries |
| GPU driver | recent | Only to render on a GPU: NVIDIA 570+ (any GPU with Vulkan ray tracing). No Vulkan SDK is needed: the build gets the Vulkan headers and the shader compiler from vcpkg |
| CUDA Toolkit | 13.0+ (or 12.8+) | Optional: builds the CUDA backend, for NVIDIA GPUs without ray tracing. Needs no NVIDIA GPU to build. `winget install Nvidia.CUDA` (about 3 GB); the build finds it through `CUDA_PATH` |

Install from a PowerShell window:

1. Build Tools with the C++ workload (skip if you already have Visual Studio 2022 or 2026 with "Desktop development with C++"):
   ```powershell
   winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   ```
2. Git:
   ```powershell
   winget install --id Git.Git -e
   ```
3. Close and reopen PowerShell so `git` is on the PATH, then clone vcpkg to `C:\dev\vcpkg`:
   ```powershell
   git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
   ```
4. Bootstrap vcpkg:
   ```powershell
   C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
   ```
5. Tell Penumbra where vcpkg is:
   ```powershell
   [Environment]::SetEnvironmentVariable('VCPKG_ROOT', 'C:\dev\vcpkg', 'User')
   ```
6. Close and reopen PowerShell. `$env:VCPKG_ROOT` now prints `C:\dev\vcpkg`.

### Ubuntu 22.04 and Debian 12 (x64)

| Tool | Version | Notes |
| --- | --- | --- |
| GCC | 12+ | `gcc-12` / `g++-12` (Ubuntu 22.04's default GCC 11 is not used) |
| CMake | 3.26+ | Ubuntu 22.04 ships 3.22 and Debian 12 ships 3.25, so install the official build below |
| Ninja, Git, curl, zip, unzip, tar, pkg-config | distro versions | |
| vcpkg | current | |
| GPU driver | recent | Only to render on a GPU: the NVIDIA proprietary driver 570+ (`sudo ubuntu-drivers install`), or Mesa 23.1+ for AMD. No Vulkan SDK is needed. In Docker, start the container with `-e NVIDIA_DRIVER_CAPABILITIES=all` (the default "compute,utility" hides Vulkan) |
| CUDA Toolkit | 13.0+ (or 12.8+) | Optional: builds the CUDA backend, for NVIDIA GPUs without ray tracing. Install the toolkit only, never the `cuda` metapackage (it replaces the driver); commands below |

Run these in a terminal:

1. Compiler and tools:
   ```bash
   sudo apt update && sudo apt install -y build-essential gcc-12 g++-12 ninja-build git curl zip unzip tar pkg-config
   ```
2. CMake 3.31.6, official build, installed to `/opt/cmake`:
   ```bash
   curl -fsSL https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz -o /tmp/cmake.tar.gz
   ```
3. Unpack it:
   ```bash
   sudo mkdir -p /opt/cmake && sudo tar -xzf /tmp/cmake.tar.gz -C /opt/cmake --strip-components=1
   ```
4. Put it first on the PATH (new terminals pick this up):
   ```bash
   echo 'export PATH=/opt/cmake/bin:$PATH' >> ~/.bashrc
   ```
5. Clone vcpkg to `~/vcpkg`:
   ```bash
   git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
   ```
6. Bootstrap vcpkg:
   ```bash
   ~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
   ```
7. Tell Penumbra where vcpkg is:
   ```bash
   echo 'export VCPKG_ROOT=$HOME/vcpkg' >> ~/.bashrc
   ```
8. Load the new settings into this terminal:
   ```bash
   source ~/.bashrc
   ```
   `cmake --version` now prints `3.31.6` and `echo $VCPKG_ROOT` prints your vcpkg folder.

Optional, the user interface (on a desktop; servers and rented containers run headless and skip this):

- Install the X11 development packages the UI's window library needs:
  ```bash
  sudo apt install -y libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
  ```
  `./scripts/build.sh` builds the UI when a display is present and says which it chose.

Optional, the CUDA toolkit (for the CUDA backend; the toolkit only, it does not touch the driver):

9. Download NVIDIA's repository key for this distribution (Ubuntu 22.04/24.04, Debian 12):
   ```bash
   . /etc/os-release && wget -O /tmp/cuda-keyring.deb "https://developer.download.nvidia.com/compute/cuda/repos/${ID}${VERSION_ID//./}/x86_64/cuda-keyring_1.1-1_all.deb"
   ```
10. Add the repository:
    ```bash
    sudo dpkg -i /tmp/cuda-keyring.deb
    ```
11. Install the toolkit:
    ```bash
    sudo apt update && sudo apt install -y cuda-toolkit-13-0
    ```
12. Put nvcc on the PATH:
    ```bash
    echo 'export PATH=/usr/local/cuda/bin:$PATH' >> ~/.bashrc && source ~/.bashrc
    ```
    `nvcc --version` now prints `release 13.0`.

## Build

### One command (recommended)

The build scripts check the prerequisites, configure, build, run the tests and a smoke test, and print where
the binary is. The first build on a machine also compiles the C++ libraries through vcpkg: expect 20 to 60
minutes, mostly Embree. Later builds reuse them and take minutes. The scripts run one compile job per core, but
at most one per 2 GB of free RAM, and print the number they chose.

**Windows** (any PowerShell window; the script sets up the Visual Studio x64 environment itself). Use a short
folder such as `C:\dev`: MSVC fails on paths longer than 260 characters.

1. Clone Penumbra into the current folder:
   ```powershell
   git clone https://github.com/NTFespolion307/Penumbra.git
   ```
2. Enter the clone:
   ```powershell
   cd Penumbra
   ```
3. Build:
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\build.ps1
   ```
   Success ends with `Built: ...\Penumbra\build\windows-msvc-release\bin\penumbra.exe` in green.

**Linux:**

1. Clone Penumbra into the current folder:
   ```bash
   git clone https://github.com/NTFespolion307/Penumbra.git
   ```
2. Enter the clone:
   ```bash
   cd Penumbra
   ```
3. Build:
   ```bash
   ./scripts/build.sh
   ```
   Success ends with `Built: .../Penumbra/build/linux-gcc-release/bin/penumbra`.

Script options: `-Config debug` / `--debug` for a debug build, `-NoTests` / `--no-tests` to skip the tests,
`-Quick` / `--quick` to run only the quick tests (unit, smoke and lint, under a minute), `-Clean` / `--clean` to
start from an empty build folder. The tests run several at a time, as many as the machine's cores allow; the full
suite takes a few minutes. On Linux the user interface is built when a display is
present (`DISPLAY` or `WAYLAND_DISPLAY`) and skipped on headless machines; `--ui` / `--no-ui` force it. Windows
always builds it.

### Manual CMake presets

Use this if you want to drive CMake yourself. On Windows, run these in an **x64** developer shell: open
"x64 Native Tools Command Prompt for VS" from the Start menu (the plain "Developer PowerShell" defaults to 32-bit).

| Preset | OS | Compiler |
| --- | --- | --- |
| `windows-msvc-release`, `windows-msvc-debug` | Windows | MSVC (x64) |
| `linux-gcc-release`, `linux-gcc-debug` | Linux | GCC 12 |

1. Configure, in the `Penumbra` folder (replace the preset name to suit):
   ```
   cmake --preset windows-msvc-release
   ```
2. Build:
   ```
   cmake --build --preset windows-msvc-release
   ```
3. Test; success ends with `100% tests passed`:
   ```
   ctest --preset windows-msvc-release -LE "large|gpu"
   ```
4. On a machine with an NVIDIA GPU, run the GPU tests (each GPU backend against the CPU, and every shading
   function compared with its CPU original); success ends with `100% tests passed`:
   ```
   .\scripts\test-gpu.ps1        # Linux: ./scripts/test-gpu.sh
   ```
   On a GPU without Vulkan ray tracing, run only the CUDA tests:
   `.\scripts\test-gpu.ps1 -Label cuda` (Linux: `./scripts/test-gpu.sh release cuda`).

The binary is `build/<preset>/bin/penumbra` (`penumbra.exe` on Windows).

### Build options

| CMake option | Default | Meaning |
| --- | --- | --- |
| `PEN_ENABLE_VULKAN` | `ON` | Vulkan RT backend (the UI swapchain from M10). `OFF` builds a CPU-only Penumbra |
| `PEN_ENABLE_CUDA` | `ON` if the CUDA toolkit is found | CUDA backend. Nothing links against CUDA: the driver is loaded when Penumbra starts, so the binary also runs on machines without NVIDIA |
| `PEN_ENABLE_OPTIX` | `OFF` | OptiX backend and denoiser *(planned, M13, optional)* |
| `PEN_ENABLE_UI` | `ON` | The user interface (M10); off for headless servers (`build.sh` turns it off when there is no display). `--terminal` and `--worker` never need display libraries either way |
| `PEN_CUDA_ARCHS` | `75;80;86;89;90;120` | CUDA architectures to compile for. Newer GPUs run the embedded PTX. CUDA 13 cannot build for GTX 10-series (61) or V100 (70); for those, build with CUDA 12.x and add the architecture, e.g. `-DPEN_CUDA_ARCHS="61;75;86"` |
| `PEN_CUDA_NVCC_FLAGS` | (empty) | Extra nvcc flags for the CUDA kernels, separated by semicolons, e.g. `-DPEN_CUDA_NVCC_FLAGS="-maxrregcount=96"`. For tuning experiments; the build log shows each kernel's registers and spills |
| `PEN_BUILD_TESTS` | `ON` | Unit, smoke and lint tests |

Pass options at configure time, for example a headless server build without CUDA or UI:

```
cmake --preset linux-gcc-release -DPEN_ENABLE_CUDA=OFF -DPEN_ENABLE_UI=OFF
```

## Running on one machine

| Command | What it does |
| --- | --- |
| `penumbra` | Opens the UI as controller: it hosts on the LAN at once (see [Using the UI](#using-the-ui)) |
| `penumbra scene.pen.json` | Opens the UI with that scene loaded on the Render page |
| `penumbra --terminal scene.glb -o out.png` | Headless render on this machine's devices (GPUs with ray tracing, else the CPU) |
| `penumbra --terminal scene.glb --res 720p --spp 16 -o preview.png` | Quick preview |
| `penumbra --terminal scene.pen.json --res 4k --spp 1024 -o final.exr` | 4K final with settings from a `.pen.json` |
| `penumbra --terminal scene.glb --integrator debug:normal -o normals.png` | Debug views: `ao`, `debug:normal`, `debug:albedo`, `debug:depth` |
| `penumbra --terminal --resume final.session.json` | Continue an interrupted render (only output, logging and device flags may change) |
| `penumbra --terminal scene.glb --res 20k --region 0,0,10240,11520 -o left.exr` | Render part of the image; the `.penfilm` is kept for `merge` |
| `penumbra merge left.penfilm right.penfilm -o full.exr` | Combine partial films (regions or `--sample-range` parts) into one image |
| `penumbra --list-devices` | Lists the devices this machine can render with |
| `penumbra --diagnose` | Prints OS, CPU, RAM, devices, folders and free disk. **Paste this into bug reports.** |
| `penumbra --version` | Prints version, git hash and protocol version |
| `penumbra --benchmark` | Measures each device alone on the scenes in `./scenes` (see [Benchmark](#benchmark)) |

### Benchmark

Run `penumbra --benchmark` from the Penumbra folder. It renders each scene in `scenes/` on each selected device
alone: `cornell` and `spheres`, plus `sponza` and `intel-sponza` once `scripts/get-scenes` has downloaded them.
Each run is 30 s at 1080p, through the same path as a normal render. It prints a table and writes
`penumbra-benchmark.json` and `penumbra-benchmark.md` (change the name with `-o name.json`).

- `penumbra --benchmark --devices 0:vulkan,0:cuda` compares both backends on GPU 0.
- `penumbra --benchmark scene.pen.json --res 4k --time-limit 2m` benchmarks your own scene.

| Column | Meaning |
| --- | --- |
| M samples/s (rendering) | Samples per second while the device was rendering: the device's speed |
| M samples/s (window) | Samples per second from its first chunk to its last, idle gaps included |
| Busy % | Rendering time over that window. Below ~95% means the device waits between chunks |
| Upload + build (s) | Scene upload and acceleration-structure build on the device; not in the two rates |
| Placement | Memory tier and how much of the device memory the scene took |

Settings come from four layers, highest priority first: command-line flags, the `render` section of a
`.pen.json`, your user config file, then built-in defaults. The user config file holds personal defaults using
the same keys as the CLI reference (for example `{"tonemap": "filmic", "worker_name": "gpu-server-1"}`):

| OS | User config | Logs |
| --- | --- | --- |
| Windows | `%APPDATA%\Penumbra\config.json` | `%LOCALAPPDATA%\Penumbra\logs` |
| Linux | `~/.config/penumbra/config.json` | `~/.local/state/penumbra/logs` |

The join code is never read from or written to the config file.

**Output.** EXR (linear, half or float, tiled), PNG (8/16-bit), JPEG, TIFF (8/16-bit display, 32-bit linear
float; BigTIFF above 4 GB) and WebP. Until AgX/Filmic/ACES arrive in M11, display formats use the sRGB curve
with clipping. Images are written in bands of 256 rows, so even a 160K output never sits in memory whole. With
`--region` the output file contains only that region. On the controller machine the CPU leaves 2 threads free
unless you pass `--threads`.

**Film, checkpoints and resume.** Every render keeps its film in `<output name>.penfilm` next to the output (or
in `--film-dir`), plus `<output name>.session.json`. Both are written at every checkpoint (`--checkpoint`,
default 10 minutes) and deleted when the render completes; `--region` and `--sample-range` renders keep their
film for `merge`.

- **Ctrl+C:** stops within seconds, writes a checkpoint and the image so far (unrendered tiles are black), and
  exits with code 5. Press Ctrl+C twice to quit immediately.
- **Crash or power loss:** at most the tiles since the last checkpoint are rendered again.
- **Resume:** `penumbra --terminal --resume <output name>.session.json`. It refuses when the scene files or the
  settings that change pixels differ, or when Penumbra was rebuilt from another commit in between, so tiles
  from different builds never mix.

Before rendering, Penumbra checks that the film and output fit on disk and exits with code 4 if not; if the disk
fills up during the render, it checkpoints and exits with code 4 too.

**How the work is split.** Each tile's samples are cut into fixed chunks (a few million samples each), and every
device pulls chunks as it finishes them, two at a time. Films kept in RAM refine the whole image pass by pass, so
an early look shows the full frame; disk films finish one tile after another. Chunks of a tile are always
combined in the same order, so the image is identical to the bit whatever the number of devices, failures or
resumes. A device that fails hands its chunks back to the others; a chunk that takes more than 10 times the
usual time is handed out again, and the first result wins. Near the end, when nothing is left to hand out, an idle
device also takes a copy of the oldest chunk still out on a slower device (tail duplication), so the last minutes
do not wait on the slowest GPU or host.

- **Render for a fixed time:** `--spp 0 --time-limit 2h` adds passes until the deadline (needs the film in RAM).
- **Pick devices:** `--devices 0` renders on GPU 0 only, `--devices 0,cpu` on GPU 0 and the CPU together,
  `--devices cpu` on the CPU only (numbers as `penumbra --list-devices` prints them). A GPU number may name its
  backend: `--devices 0:vulkan,1:cuda` (the same GPU may be listed twice, `0:vulkan,0:cuda`, mainly for testing).
  `--devices cpu,cpu,cpu,cpu` splits the CPU into four devices (mainly for testing). With GPUs, the CPU device
  leaves one thread per GPU free to feed it.

**Quick test right after cloning.** The repository ships three small scenes in `scenes/`: a Cornell box,
material spheres on an HDRI, and an alpha-cutout scene. From the repository folder, with the binary path the build
script printed:

1. Render the Cornell box (a few seconds on a GPU, under a minute on most CPUs):
   ```
   build/windows-msvc-release/bin/penumbra.exe --terminal scenes/cornell.pen.json --spp 256 -o cornell.png
   ```
   On Linux: `build/linux-gcc-release/bin/penumbra --terminal scenes/cornell.pen.json --spp 256 -o cornell.png`
2. The spheres on an HDRI (720p): `penumbra --terminal scenes/spheres.pen.json --spp 256 -o spheres.png`
3. Several machines: host `scenes/cornell.pen.json` with `--terminal --host`, and join from another machine as in
   [Rendering with several machines](#rendering-with-several-machines).

**A large textured scene: Sponza.** For a realistic load (262,000 triangles, 69 textures, 25 materials with
alpha-cut plants, lit by a sunny HDRI sky) download Sponza once; it is about 58 MB, so it is not in the repository.
Run these from the repository folder:

1. Download it (every file is checked against `scenes/sponza.sha256`; running it again only fills in what is
   missing):
   ```
   .\scripts\get-scenes.ps1
   ```
   On Linux: `./scripts/get-scenes.sh`. Success: the last lines say `Ready: scenes/sponza.pen.json`.
2. Render it from the courtyard camera (1080p; minutes on a GPU, much longer on a CPU):
   ```
   build/windows-msvc-release/bin/penumbra.exe --terminal scenes/sponza.pen.json --spp 1024 -o sponza.png
   ```
   On Linux: `build/linux-gcc-release/bin/penumbra --terminal scenes/sponza.pen.json --spp 1024 -o sponza.png`.
   In the UI, open `scenes/sponza.pen.json` on the Render page.

Sponza is Crytek's model (Cryengine Limited License Agreement) as published in the
[Khronos glTF sample assets](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Sponza);
the sky is [Kloofendal 48d Partly Cloudy (Pure Sky)](https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky)
by Greg Zaal and Jarod Guest (CC0, Poly Haven). Both are downloaded from their publishers, not redistributed here.
The script adds a camera ("Courtyard") to a copy of the glTF, `scenes/sponza/courtyard.gltf`.

**A heavy production scene: Intel's New Sponza.** The modern remake of Sponza: 4K PBR textures (72 of them, about
4.5 GB once loaded), photogrammetry geometry, 24 lights, 6 cameras and an HDRI sky. It is a 4 GB download (2.8 GB
is kept; the 3ds Max, FBX and USD versions in the archive are left out) and it is the scene the memory tiers
are tested with: on an 8 GB GPU its textures do not all fit in VRAM.

1. Download it (checked against its SHA-256):
   ```
   .\scripts\get-scenes.ps1 -Scene intel-sponza
   ```
   On Linux: `./scripts/get-scenes.sh intel-sponza`. Success: the last lines say `Ready: scenes/intel-sponza.pen.json`.
2. Render it:
   ```
   build/windows-msvc-release/bin/penumbra.exe --terminal scenes/intel-sponza.pen.json --spp 1024 -o intel-sponza.png
   ```
   Other views: add `--camera PhysCamera002` (up to `PhysCamera006`).

Sponza 2022 by Frank Meinl and Anton Kaplanyan ([Intel Sample Library](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html)),
CC BY 4.0, "for personal use and educational use, limited commercial use for marketing and print purposes";
reference photos by Katica Putica and Princino.photo. The full credits and license are in
`scenes/intel-sponza/main_sponza/credits_license.txt` after the download. Downloaded from Intel, not
redistributed here.

**Test scenes and image comparison.** The build also makes two tools next to `penumbra`:

1. Write all test scenes (the three above plus the furnace spheres and a big-texture scene) into a folder:
   ```
   penumbra-scenegen scenes
   ```
2. Render the Cornell box:
   ```
   penumbra --terminal scenes/cornell.pen.json --spp 1024 -o cornell.png
   ```
3. Compare two renders (relMSE and max difference; `--exact` for bitwise):
   ```
   penumbra-diff a.exr b.exr
   ```

### Using the UI

Start `penumbra` with no arguments (or double-click it). The sidebar has five pages:

| Page | What it is for |
| --- | --- |
| **Dashboard** | The join code and address to give workers, every connected host with its GPUs, cluster CPU, RAM and GPU use, recent events |
| **Hosts** | One row per machine (this one first); click a row for its GPUs, temperatures, scene cache and log, and to rename, kick or switch off a host or single GPU |
| **Render** | Open a scene (`.glb`, `.gltf`, `.obj`, `.pen.json`), check what was found in it, pick camera, resolution preset, samples, sky and atmosphere (sun position, haze, fog), output file and format, see a quick framing preview and the time estimate, then **Start render**. A file name without a folder is saved in the output folder from Settings (default `Pictures\Penumbra`, `~/Pictures/Penumbra` on Linux) |
| **Viewport** | The image as it renders, with each tile coloured by the host that rendered it; zoom (mouse wheel) and pan (drag), exposure, **Pause**, **Save image now**, **Stop** (keeps the checkpoint for `--resume`) |
| **Settings** | LAN and internet hosting, "ask before accepting new workers", the join code (**Copy**, **Regenerate**), the port, this machine's devices (which GPUs render, whether the CPU renders too, CPU threads kept free for the UI), default output and film folders, **Open logs folder**, **Run diagnostics**, and **Join a render** |

**Join a render** (Settings) turns this machine into a worker of another controller: enter its address and join
code, press **Join**. Hosting on this machine stops while joined; the screen shows the controller, the scene
download, each device's use and tiles, and the upload rate. **Leave** hands unfinished work back and hosting starts
again. Internet hosting is off each time the UI starts; switch it on in Settings when workers come from outside
your network.

## Rendering with several machines

Hosting and joining work the same in the UI and headless; the UI's pages are described in
[Using the UI](#using-the-ui). Workers on another OS than the controller give the same image within relMSE 1e-10 (different math
libraries), workers on the same OS bit for bit.

**Hosting.** In the UI, LAN hosting starts with the app and the Dashboard shows the join code and the address to
give workers; internet hosting is switched on in Settings. Headless, add `--host`:

```
penumbra --terminal --host scene.glb -o out.exr --res 8k
```

The controller prints its addresses and a join code like `K7QD-M2XR-9FHT-4WNB`. The code is saved in your user
data folder (`%LOCALAPPDATA%\Penumbra\join_code.txt` on Windows, `~/.local/share/penumbra/join_code.txt` on Linux)
and stays the same across restarts; delete that file to get a new one. `--code` uses a code of your choice
instead. The code is never sent over the network: both sides derive the encryption key from it (TLS 1.3), so a
machine with the wrong code cannot connect, and an address that tries more than 5 wrong codes in a minute is
ignored for 10 minutes. Add `--approve-workers` to be asked before each new worker joins.

**Joining.** On each worker machine:

```
penumbra --worker --join 192.168.1.20 --code K7QD-M2XR-9FHT-4WNB
```

Set the code in the `PENUMBRA_CODE` environment variable instead of `--code` to keep it out of shell history.
Add `--stay` on always-on machines: the worker then waits for the next render and reconnects on its own whenever
the controller goes away (it retries after 1, 2, 4 and then every 5 seconds, forever; Ctrl+C stops it; an attempt
gives up after 10 seconds without an answer). In the UI, use
**Join a render**. Without `--stay`, a worker exits when the render ends (exit code 0) or the connection drops
(exit code 7).

**The scene.** Workers need nothing but Penumbra: when a render starts, the controller packages every file the
scene reads (the `.glb`/`.gltf` and its buffers and textures, OBJ materials and textures, the HDRI) and each worker
downloads the package once. Workers keep packages in their scene cache (`%LOCALAPPDATA%\Penumbra\cache\scenes` on
Windows, `~/.cache/penumbra/scenes` on Linux, or `--cache-dir`, up to `--cache-size`, default 20G), so rendering the
same scene again, with any settings, downloads nothing. An interrupted download resumes where it stopped. A worker
without enough disk or RAM for the scene says so in the controller's log and the render continues without it.
`--upload-limit 10M` on the controller caps how fast scenes are sent (bytes per second, all workers together).

**LAN vs internet.** Workers connect *to* the controller, so only the controller needs a reachable port, TCP 7420
(change it with `--port`).

- **LAN:** join with the controller's LAN address (e.g. `192.168.1.20`).
- **Internet:** enable internet hosting; Penumbra tries UPnP to open the port on your router (`--no-upnp` to
  skip). If UPnP is off on the router, forward TCP 7420 to the controller by hand, then join with your public IP.
- **Carrier-grade NAT** (no public IPv4, common on mobile and some fibre ISPs): port forwarding cannot work. Put
  the controller and workers on a VPN such as Tailscale or WireGuard and join with the controller's VPN address.
  With Tailscale: install it on every machine (tailscale.com/download), sign in with the same account, then join
  with the controller's Tailscale address (the controller lists it as a VPN address when it starts hosting, or run
  `tailscale ip -4` on it). Nothing needs to be forwarded on any router.

When hosting starts, the controller prints every address workers can use (LAN, VPN, public) with the full
`--join` command, and the result of UPnP: forwarded (with the public address), no UPnP router, or carrier-grade NAT
(use a VPN). Add `--no-upnp` to never touch the router.

**Firewall.**

- Windows: allow Penumbra when Windows Defender Firewall asks the first time the controller listens.
- Linux with ufw: run `sudo ufw allow 7420/tcp` on the controller.

**Example: a 160K render across three machines.** A GPU server hosts (the film for 160K needs about 242 GB of
free disk on the controller), and two more machines join over the internet through Tailscale:

1. On the GPU server (Linux), host the render with the film on a big disk:
   ```bash
   penumbra --terminal --host city.pen.json --res 160k --spp 256 -o /data/city.exr --film-dir /data/films
   ```
2. On the gaming PC (Windows):
   ```powershell
   penumbra --worker --join 100.101.102.103 --code K7QD-M2XR-9FHT-4WNB
   ```
3. On a second server (Linux), always on:
   ```bash
   penumbra --worker --join 100.101.102.103 --code K7QD-M2XR-9FHT-4WNB --stay
   ```

If the controller crashes, run `penumbra --terminal --resume /data/films/city.session.json`; `--stay` workers
reconnect by themselves.

All machines must run the same Penumbra version and git commit (see [Updating](#updating)).

### Rented GPU containers (vast.ai, RunPod and similar)

Rented containers usually may not create network interfaces, so Tailscale runs there in *userspace* mode:
connections **to** the container over Tailscale work as usual, but programs in the container cannot open
connections **out** over Tailscale by themselves, only through a proxy that Tailscale provides. So:

- **The container hosts the render** (controller): nothing extra. Other machines join its Tailscale address.
- **The container joins a render** (worker): a small relay, `socat`, passes Penumbra's connection through
  Tailscale's proxy. Steps 5 and 6 below.

Run these in the container's shell, as root (containers usually log you in as root). Containers rented by the day
start empty, so repeat them on every new container.

1. Install Tailscale:
   ```bash
   curl -fsSL https://tailscale.com/install.sh | sh
   ```
   Success: the last lines say the installation is complete.
2. Start Tailscale in userspace mode, with its outbound proxy on port 1055:
   ```bash
   mkdir -p /var/run/tailscale /var/lib/tailscale && (tailscaled --tun=userspace-networking --outbound-http-proxy-listen=localhost:1055 --state=/var/lib/tailscale/tailscaled.state > /var/log/tailscaled.log 2>&1 &)
   ```
   Success: nothing is printed; `pgrep -x tailscaled` prints a process number.
3. Sign the container in:
   ```bash
   tailscale up --hostname=gpu-box-1
   ```
   Success: it prints a login link; open it, approve the machine, and the command prints `Success.`
   To skip the link on every new container, create a reusable auth key once in the Tailscale admin console
   (Settings → Keys) and run `tailscale up --hostname=gpu-box-1 --authkey=tskey-...` instead.
4. Print the container's Tailscale address (needed when the container hosts):
   ```bash
   tailscale ip -4
   ```
   Success: an address starting with `100.`
5. Worker only: install the relay:
   ```bash
   apt-get update && apt-get install -y socat
   ```
   Success: `socat -V` prints a version.
6. Worker only: start the relay, with the controller's Tailscale address in place of `100.101.102.103`:
   ```bash
   (socat TCP-LISTEN:7420,bind=127.0.0.1,fork,reuseaddr PROXY:127.0.0.1:100.101.102.103:7420,proxyport=1055 &)
   ```
   Success: nothing is printed; `pgrep -x socat` prints a process number.
7. Worker only: join through the relay, from the folder where you built Penumbra:
   ```bash
   ./build/linux-gcc-release/bin/penumbra --worker --join 127.0.0.1 --code K7QD-M2XR-9FHT-4WNB
   ```
   Success: the log says `joined the controller as worker N`.

Why the relay: `socat` listens on the container's own port 7420 and forwards each connection through Tailscale's
proxy (port 1055) to the controller, so the worker simply joins `127.0.0.1`. Machines with a normal Tailscale or
WireGuard install (PCs, VMs, your own servers) do not need any of this.

## CLI reference

Generated from the same option table as `penumbra --help`. Every setting can also be set by its key in a
`.pen.json` `render` section or in the user config file; "CLI only" flags cannot. Run `penumbra --help` for the
full text on your machine.

<!-- BEGIN CLI REFERENCE: generated by `penumbra --help-markdown`, do not edit by hand -->
| Group | Flag | Values | Default | Example | Config / `.pen.json` key | Description |
| --- | --- | --- | --- | --- | --- | --- |
| General | `--terminal` |  | off | `penumbra --terminal scene.glb -o out.png` | CLI only | Headless controller: no window, no display server needed |
| General | `--config` | path | per-OS user config | `penumbra --config defaults.json` | CLI only | Defaults file to use |
| General | `--log-level` | trace, debug, info, warn, error, off | info | `penumbra --log-level debug` | `log_level` | Log verbosity for console and log file |
| General | `--log-file` | path | per-OS log folder | `penumbra --log-file render.log` | `log_file` | Write the log here instead of the per-OS log folder |
| General | `--progress` | bar, plain, json | bar | `penumbra --progress json` | `progress` | Progress output style; json prints one object per second on stdout |
| General | `--scene`, `<scene>` | .glb, .gltf, .obj, .pen.json | none | `penumbra scene.glb` | `scene` | Scene to load (also accepted as the first positional argument) |
| Render | `--res` | 120p, 240p, 360p, 480p, 720p, 1080p, 2k, 4k, 6k, 8k, 10k, 15k, 20k, 40k, 60k, 80k, 160k, or WxH | 1080p | `penumbra --res 4k` | `res` | Output resolution |
| Render | `--scale` | 0.01-1.0 | 1.0 | `penumbra --scale 0.25` | `scale` | Render the resolution at a fraction of its size (quick tests) |
| Render | `--spp` | 1-4294967295 (0 with --time-limit) | 128 | `penumbra --spp 1024` | `spp` | Samples per pixel |
| Render | `--max-bounces` | 0-1024 | 12 | `penumbra --max-bounces 32` | `max_bounces` | Path depth cap (0 = direct light only) |
| Render | `--max-diffuse` | 0-1024 | --max-bounces | `penumbra --max-diffuse 4` | `max_diffuse` | Diffuse bounce cap |
| Render | `--max-glossy` | 0-1024 | --max-bounces | `penumbra --max-glossy 8` | `max_glossy` | Glossy bounce cap |
| Render | `--max-transmission` | 0-1024 | --max-bounces | `penumbra --max-transmission 16` | `max_transmission` | Transmission bounce cap |
| Render | `--integrator` | path, ao, debug:normal, debug:albedo, debug:depth, debug:bvh | path | `penumbra --integrator ao` | `integrator` | Light transport algorithm |
| Render | `--no-rr` |  | off | `penumbra --no-rr` | `no_rr` | Disable Russian roulette path termination |
| Render | `--ao-distance` | scene units, 0 = automatic | 0 | `penumbra --ao-distance 0.5` | `ao_distance` | Occlusion distance for --integrator ao |
| Render | `--seed` | 0-4294967295 | 0 | `penumbra --seed 7` | `seed` | Deterministic sampling seed |
| Render | `--camera` | camera name | first in scene | `penumbra --camera Camera.001` | `camera` | Camera to render from |
| Render | `--time-limit` | duration (2h30m, 45m, 90s) or none | none | `penumbra --time-limit 2h30m` | `time_limit` | Stop and write the output at the deadline |
| Devices | `--backend` | auto, vulkan, cuda, cpu, optix | auto | `penumbra --backend cuda` | `backend` | Ray tracing backend |
| Devices | `--devices` | all, gpu, or a list like 0,1:cuda,cpu (GPU numbers from --list-devices, optionally :vulkan or :cuda; repeat cpu for several CPU slots) | all | `penumbra --devices 0,cpu` | `devices` | Which local devices render (controller or worker) |
| Devices | `--threads` | 0-4096, 0 = all cores | 0 | `penumbra --threads 8` | `threads` | CPU worker threads |
| Devices | `--tile-size` | 16-4096 | 256 | `penumbra --tile-size 128` | `tile_size` | Work item tile size in pixels |
| Devices | `--vram-limit` | size (6G, 512M) or auto | auto | `penumbra --vram-limit 6G` | `vram_limit` | VRAM budget per GPU (default: free VRAM minus 10%) |
| Devices | `--ram-limit` | size (24G) or auto | auto | `penumbra --ram-limit 24G` | `ram_limit` | RAM budget (default: 75% of physical RAM) |
| Devices | `--no-texture-downsize` |  | off | `penumbra --no-texture-downsize` | `no_texture_downsize` | Forbid memory tier 2 (downsized textures) |
| Output | `-o`, `--output` | file path; the extension picks the format | render.png | `penumbra -o out.exr` | `output` | Output image |
| Output | `--format` | png, exr, jpg, tiff, webp | from extension | `penumbra --format exr` | `format` | Output encoding (must match the extension if there is one) |
| Output | `--bit-depth` | 8, 16, 32 (per format) | png/jpg/webp 8, tiff/exr 16 | `penumbra --bit-depth 32` | `bit_depth` | Bits per channel |
| Output | `--denoise` | none, oidn, optix | none | `penumbra --denoise oidn` | `denoise` | Denoiser |
| Output | `--tonemap` | agx, filmic, aces, none | agx | `penumbra --tonemap filmic` | `tonemap` | Display transform |
| Output | `--exposure` | EV, -20 to 20 | 0 | `penumbra --exposure 1.5` | `exposure` | Exposure adjustment before tonemapping |
| Output | `--aov` | albedo, normal, depth (comma list) | none | `penumbra --aov albedo,normal` | `aov` | Extra passes |
| Film | `--resume` | session file |  | `penumbra --resume render.session.json` | CLI only | Continue an interrupted render (add --terminal for headless) |
| Film | `--film-dir` | folder | output folder | `penumbra --film-dir D:\films` | `film_dir` | Where the film and session file live |
| Film | `--checkpoint` | duration, 10s to 7d | 10m | `penumbra --checkpoint 5m` | `checkpoint` | Checkpoint period |
| Film | `--region` | x0,y0,x1,y1 (x1, y1 exclusive) or none | none | `penumbra --region 0,0,1024,1024` | `region` | Render only part of the image (for merge) |
| Film | `--sample-range` | a-b (inclusive) or none | none | `penumbra --sample-range 0-511` | `sample_range` | Render only these samples (for merge) |
| Controller | `--host` |  | off | `penumbra --terminal --host scene.glb` | CLI only | Accept workers (headless controller; needs --terminal) |
| Controller | `--port` | 1-65535 | 7420 | `penumbra --port 7421` | `port` | Listen port |
| Controller | `--bind` | address | all interfaces | `penumbra --bind 192.168.1.20` | `bind` | Listen address |
| Controller | `--approve-workers` |  | off | `penumbra --approve-workers` | `approve_workers` | Ask before accepting each new worker |
| Controller | `--max-workers` | 1-4096 | 64 | `penumbra --max-workers 8` | `max_workers` | Connection limit |
| Controller | `--no-upnp` |  | off | `penumbra --no-upnp` | `no_upnp` | Never try UPnP |
| Controller | `--no-local` |  | off | `penumbra --no-local` | `no_local` | Coordinate only, render nothing on this machine |
| Controller | `--upload-limit` | bytes per second (10M) or unlimited | unlimited | `penumbra --upload-limit 10M` | `upload_limit` | Cap scene upload bandwidth |
| Worker | `--worker` |  | off | `penumbra --worker --join 192.168.1.20 --code K7QD-M2XR-9FHT-4WNB` | CLI only | Run as a headless worker |
| Worker | `--join` | address[:port] |  | `penumbra --join 203.0.113.5:7420` | CLI only | Controller to join (LAN IP, public IP, VPN IP or host name) |
| Worker | `--code` | XXXX-XXXX-XXXX-XXXX | env PENUMBRA_CODE | `penumbra --code K7QD-M2XR-9FHT-4WNB` | CLI only | Join code. Workers need it; a controller uses it as a fixed code instead of the saved one |
| Worker | `--worker-name` | text | host name | `penumbra --worker-name gpu-server-1` | `worker_name` | Name shown in the controller UI |
| Worker | `--stay` |  | off | `penumbra --stay` | `stay` | Keep serving renders after one completes and reconnect on its own |
| Worker | `--cache-dir` | folder | per-OS cache folder | `penumbra --cache-dir /data/penumbra` | `cache_dir` | Scene cache folder |
| Worker | `--cache-size` | size, at least 1G | 20G | `penumbra --cache-size 100G` | `cache_size` | Scene cache size before least-recently-used eviction |
| Info | `--version` |  |  | `penumbra --version` | CLI only | Print version and git hash, then exit |
| Info | `--list-devices` |  |  | `penumbra --list-devices` | CLI only | List this machine's render devices, then exit |
| Info | `--diagnose` |  |  | `penumbra --diagnose` | CLI only | Print OS, devices, paths and free disk for bug reports |
| Info | `--benchmark` |  |  | `penumbra --benchmark --devices 0:vulkan,0:cuda` | CLI only | Render each scene on each selected device alone (the scene given, else ./scenes; 1080p, 30 s each unless --res, --time-limit or --spp is given), then print samples/s and busy % and write -o (default penumbra-benchmark.json) plus a .md table |
| Subcommand | `merge <films...> -o <output>` |  |  | `penumbra merge a.penfilm b.penfilm -o out.exr` | CLI only | Combine partial films into one output image |
<!-- END CLI REFERENCE -->

**Exit codes:** 0 success, 1 bad arguments (also: mode not built yet in this milestone), 2 scene load error,
3 no usable device, 4 out of disk, 5 interrupted (checkpoint written), 6 rejected by controller, 7 connection
lost.

## Supported formats

**What Penumbra reads:**

| What | Formats | Notes |
| --- | --- | --- |
| Scene | glTF 2.0: `.glb` (binary, recommended), `.gltf` (with its `.bin` and texture files); Wavefront `.obj` with `.mtl` | glTF is the main format (Blender's exporter, see [Scenes from Blender](#scenes-from-blender)); OBJ is for simple test meshes |
| Scene settings | `.pen.json` | Wraps a scene with the HDRI, camera choice and render settings |
| glTF extensions | `KHR_materials_transmission`, `_ior`, `_clearcoat`, `_sheen`, `_specular`, `_emissive_strength`, `_anisotropy`, `KHR_texture_transform`, `KHR_lights_punctual`, `EXT_mesh_gpu_instancing` | Other extensions log a warning and fall back to the base PBR material. `KHR_materials_volume` renders as solid glass. Draco-compressed meshes are skipped with a warning (export without Draco) |
| Textures | PNG, JPEG, BMP, TGA, Radiance `.hdr`, OpenEXR `.exr` | sRGB or linear as glTF specifies |
| Environment (HDRI) | Radiance `.hdr`, OpenEXR `.exr` (also PNG/JPEG, treated as low dynamic range) | Equirectangular (latitude-longitude) maps |

**What Penumbra writes:** PNG (`.png`), OpenEXR (`.exr`), JPEG (`.jpg`, `.jpeg`), TIFF (`.tif`, `.tiff`) and WebP
(`.webp`); the output file's extension picks the format. Bit depths and size limits are in
[Output formats and limits](#output-formats-and-limits). Films (`.penfilm`) and session files (`.session.json`)
are Penumbra's own, for `--resume` and `merge`.

## Resolutions, backends, memory and formats

### Resolution presets

All presets are 16:9 with even sizes. Any other size works as `--res WxH` (16 to 163840 px per side);
`--scale 0.25` renders at a fraction for tests.

| `--res` | Width × height | Megapixels | Film size (16 B/px) | Film lives in |
| --- | --- | --- | --- | --- |
| `120p` | 214 × 120 | 0.03 | 0.4 MB | RAM |
| `240p` | 426 × 240 | 0.10 | 1.6 MB | RAM |
| `360p` | 640 × 360 | 0.23 | 3.7 MB | RAM |
| `480p` | 854 × 480 | 0.41 | 6.6 MB | RAM |
| `720p` | 1280 × 720 | 0.92 | 15 MB | RAM |
| `1080p` | 1920 × 1080 | 2.07 | 33 MB | RAM |
| `2k` | 2560 × 1440 | 3.69 | 59 MB | RAM |
| `4k` | 3840 × 2160 | 8.29 | 133 MB | RAM |
| `6k` | 5760 × 3240 | 18.7 | 299 MB | RAM |
| `8k` | 7680 × 4320 | 33.2 | 531 MB | RAM |
| `10k` | 10240 × 5760 | 59.0 | 0.94 GB | RAM |
| `15k` (often called 16K) | 15360 × 8640 | 133 | 2.1 GB | RAM |
| `20k` | 20480 × 11520 | 236 | 3.8 GB | RAM if the budget allows, else disk |
| `40k` | 40960 × 23040 | 944 | 15.1 GB | RAM if the budget allows, else disk |
| `60k` | 61440 × 34560 | 2,123 | 34.0 GB | Disk |
| `80k` | 81920 × 46080 | 3,775 | 60.4 GB | Disk |
| `160k` | 163840 × 92160 | 15,099 | 241.6 GB | Disk |

**Controller disk:** the film lives only on the controller, next to the output unless you pass `--film-dir`.
It always has a copy on disk (the checkpoint); "RAM" in the table means it is also kept in memory for speed,
which happens up to `40k` when the film fits in half of the RAM budget (`--ram-limit`, default 75% of RAM).
Before a render starts Penumbra checks there is room for the film and the output.

### Backends

| `--backend` | Runs on | Role |
| --- | --- | --- |
| `vulkan` | NVIDIA RTX 20+, AMD RDNA2+, Intel Arc | Default on GPUs with ray tracing; uses the RT cores |
| `cuda` | NVIDIA, compute capability 7.5+ (older with a CUDA 12 build) | Default on NVIDIA GPUs without Vulkan ray tracing (software BVH, tinybvh CWBVH); 4-19x slower than Vulkan RT on RTX 50-series GPUs (measured, `docs/BENCHMARKS.md`). Also on RTX GPUs with `--backend cuda`, and the only backend for `--integrator debug:bvh` |
| `cpu` | Any x86-64 or ARM64 CPU | Fallback and reference; joins GPUs in hybrid mode |
| `optix` *(planned, M13)* | NVIDIA RTX | Optional, only if it beats Vulkan RT |

`--backend auto` (default) picks Vulkan RT on every GPU with ray-tracing support, CUDA on NVIDIA GPUs without it,
and the CPU only when no GPU qualifies or `--devices` lists `cpu`. Every choice is printed in the log.
`penumbra --list-devices` numbers the GPUs as `--devices` takes them: ray-tracing GPUs first, then NVIDIA GPUs
that only CUDA can use; GPUs that cannot render are listed with the reason.

### Multi-GPU and memory tiers

A worker (and the controller's own machine) uses all its GPUs at once, one slot per GPU; each GPU pulls its own
work, so fast GPUs do more of the frame. Backends mix freely on one host (`--devices 0:vulkan,1:cuda,cpu`), and
with the CPU in the list it renders alongside, leaving one thread per GPU to feed it. Every GPU loads the scene at
the same time; a GPU that cannot take the scene is skipped with the reason in the log and the others render. A GPU
that fails or stops answering mid-render is removed and its work goes to the others: a removed GPU never stops
the render, and the image is the same. On NVIDIA GPUs the worker reports usage, VRAM and temperature from the
driver (NVML); `penumbra --list-devices` shows them too.

Each GPU picks a memory tier for the scene from what it really needs and what is free:

| Tier | Where the scene lives | Speed |
| --- | --- | --- |
| **0** | Everything in VRAM | Full speed |
| **1** | Geometry in VRAM; textures and the HDRI in RAM, read over PCIe | Typically 1.2–2× slower; same image |
| **2** | As 1, with the largest textures halved until they fit (the log says how much) | As 1; slightly softer textures |
| **3** | The geometry does not fit: this GPU sits the render out, the others render | — |

The VRAM budget is the GPU's free VRAM minus 10% (`--vram-limit 6G` sets a lower one); RAM for textures is 75% of
the machine's RAM less the scene itself (`--ram-limit`), shared by the machine's GPUs. `--no-texture-downsize`
makes a GPU that would need tier 2 skip the render instead. Each GPU's choice is in the log and on the Hosts page,
for example `tier 1, scene 6.1 GB of 7.2 GB VRAM, textures 2.4 GB in RAM`. If memory runs out anyway (another
program took VRAM after the start), the GPU drops one tier and tries again: running out of memory never crashes
the render. `penumbra --diagnose` also says whether a GPU's driver could keep acceleration structures in RAM (not
used: such a GPU is tier 3).

### Output formats and limits

The output extension picks the format; `--bit-depth` picks bits per channel. Impossible format and size pairs
are rejected before rendering.

| Format | Extensions | Bit depths (default first) | Size limit |
| --- | --- | --- | --- |
| PNG | `.png` | 8, 16 | none at Penumbra sizes |
| OpenEXR | `.exr` | 16 (half), 32 (float) | none at Penumbra sizes |
| JPEG | `.jpg`, `.jpeg` | 8 | 65,500 px per side (up to `60k`) |
| TIFF | `.tiff`, `.tif` | 16, 8, 32 | BigTIFF above 4 GB |
| WebP | `.webp` | 8 | 16,383 px per side (up to `15k`) |

## Scenes from Blender

**Export:** in Blender, File → Export → glTF 2.0. Choose **glTF Binary (.glb)**, and under Include tick
**Cameras** and **Punctual Lights**. Under Data → Mesh keep **Apply Modifiers** on. Materials should use the
Principled BSDF; supported extensions include transmission, IOR, clearcoat, sheen, specular, emissive strength,
anisotropy, texture transform, punctual lights and GPU instancing. Anything else is reported in the log and
falls back to base PBR. Transmissive materials render as solid glass (refracting in and out). Without a camera
in the file, Penumbra frames the whole scene automatically.

**`.pen.json`** wraps the `.glb` with what glTF cannot carry. Paths are relative to the `.pen.json`; keys in
`render` are the setting keys from the CLI reference:

```json
{
  "scene": "living_room.glb",
  "environment": { "hdri": "studio_4k.exr", "strength": 1.0, "rotation_deg": 90 },
  "camera": "Camera.001",
  "render": { "res": "4k", "spp": 512, "max_bounces": 12, "tonemap": "agx" }
}
```

Render it with `penumbra --terminal living_room.pen.json -o living_room.exr`; flags on the command line override
the `render` section.

More `.pen.json` options:

| Key | Meaning |
| --- | --- |
| `environment.hdri` | Equirectangular `.hdr` or `.exr`, mapped like Blender's Environment Texture |
| `environment.color` | `[r, g, b]` uniform sky radiance when there is no HDRI |
| `environment.strength`, `environment.rotation_deg` | Multiplier, and rotation about the up axis |
| `camera` | A camera name, or an object `{ "name", "projection", "fstop", "focus_distance", "sensor_width_mm" }` |
| `camera.projection` | `perspective`, `orthographic` or `equirectangular` (360° panorama) |
| `camera.fstop` | Depth of field: f-number (scene units in metres); `focus_distance` defaults to the scene centre |

Light units: glTF point and spot intensities (candela) and sun illuminance (lux) are divided by 683 lm/W, which
matches Blender's "Standard" export; emissive materials use emissive colour × strength as radiance.

**Atmosphere** *(M14; CPU, Vulkan and CUDA)*. An `"atmosphere"` object fills the scene with
air, haze and an optional ground fog. Each layer thins out exponentially with height, so distant objects fade and
turn blue, and light shafts form. With a `"sky"`, air and haze follow its `air` and `dust` values, so the haze
matches the sky.

```json
"atmosphere": { "altitude": 1600, "fog_density": 0.002, "fog_height": 60, "fog_base": 0 }
```

| Key | Meaning (default) |
| --- | --- |
| `air`, `haze` | Earth's air (Rayleigh, blue) and haze (Mie) as multiples of sea level; 0 turns a layer off (1, or the sky's `air` / `dust`) |
| `haze_anisotropy` | Henyey-Greenstein *g* of the haze: how strongly it scatters forward, around the sun (0.76) |
| `altitude` | Metres above sea level at y = 0; air and haze are thinner higher up (0) |
| `meters_per_unit` | Metres per scene unit, for scenes not modelled in metres (1) |
| `fog_density` | Ground fog: extinction per metre at `fog_base`; 0.004 is about 1 km visibility (0: no fog) |
| `fog_height` | Fog scale height in metres: the density falls by e every this many metres up (100) |
| `fog_base`, `fog_color`, `fog_anisotropy` | Fog base height (scene units, y), its colour `[r, g, b]` (single-scattering albedo, [1, 1, 1]) and *g* (0) |

**Physical sky** *(M14; every backend: it becomes the environment map)*. A `"sky"` object replaces the environment with a sky computed from
the sun's position, like Blender's Nishita sky: air, haze and ozone scatter sunlight over a round Earth, computed per
wavelength, with the sun's disc. It is in real units (the sun is 1361 W/m² above the air), so use an exposure of
about -6 to -10 (`--exposure -8`), as with Blender's.

```json
"sky": { "sun_elevation_deg": 12, "sun_azimuth_deg": -60, "altitude": 1600 }
```

| Key | Meaning (default) |
| --- | --- |
| `sun_elevation_deg`, `sun_azimuth_deg` | Sun height above the horizon, and its direction from -Z (glTF forward) turning towards +X (30, 0) |
| `sun_size_deg`, `sun_strength`, `sun_disc` | Sun's angular diameter (0.545), brightness of the disc only (1), and whether the disc is drawn (true) |
| `altitude` | Viewer's metres above sea level (0) |
| `air`, `dust`, `ozone` | Densities as multiples of Earth's (1 each) |
| `strength` | Multiplier for the whole sky (1) |
| `resolution` | Width of the computed sky map; its height is half (2048) |

Compared with Blender 4.2's Nishita sky at the same settings, brightness and colour agree within about 5% above
10° elevation. Near the horizon Penumbra's sky is brighter and bluer: Blender integrates the air along the view coarsely
there. Penumbra's sun disc is limb-darkened but keeps the sun's full power, about 18% more than Blender's.

## Troubleshooting

Start with `penumbra --diagnose` and include its output in any bug report.

| Problem | What to do |
| --- | --- |
| Build script: "vcpkg not found" | Follow [Prerequisites](#prerequisites) steps for vcpkg, then open a new terminal so `VCPKG_ROOT` is set. |
| Build script: "Visual Studio ... without the C++ x64 tools" | In the Visual Studio Installer, add the "Desktop development with C++" workload. |
| Windows: configure fails with `C1083 ... Invalid argument` or "Detecting CXX compiler ABI info - failed" | The folder path is too long for MSVC (260-character limit). Clone Penumbra to a short path such as `C:\dev\Penumbra`. |
| Build fails with `Killed signal terminated program cc1plus`, or a dependency build crawls on one core | The machine ran out of memory: some library files need about 2 GB per compiler. The build scripts already limit parallel jobs to available RAM / 2 GB; if it still happens, close other programs or lower the limit, e.g. `VCPKG_MAX_CONCURRENCY=4 ./scripts/build.sh` (PowerShell: `$env:VCPKG_MAX_CONCURRENCY=4` first). |
| Linux: "CMake 3.26 or newer is required" | Install the official CMake as in [Prerequisites](#prerequisites); check `which cmake` points to `/opt/cmake/bin/cmake`. |
| No Vulkan ray tracing, or the GPU is not listed | Update the GPU driver; `penumbra --list-devices` and `--diagnose` show each GPU and what it is missing. In Docker, the container needs `-e NVIDIA_DRIVER_CAPABILITIES=all`. NVIDIA GPUs without ray tracing render through CUDA when the build has it (`--diagnose` shows "CUDA build"); other GPUs without it are skipped and the CPU renders. |
| CUDA not found | At build time: install the CUDA toolkit and rebuild (CMake prints `CUDA=ON`); without it Penumbra still builds with Vulkan and CPU. At run time: `penumbra --diagnose` shows the driver and each GPU. "The driver supports CUDA 12.x, this build needs CUDA 13.x": update the NVIDIA driver (580+), or build with a CUDA 12 toolkit. "compute capability ... is older than this build's oldest target": see `PEN_CUDA_ARCHS`. |
| The UI window stays black, flickers or does not open | The UI draws with Vulkan and falls back to OpenGL 3.3 by itself when Vulkan cannot show a window; the log says which (`UI: drawing with ...`). To force OpenGL, start it with the environment variable `PENUMBRA_UI_RENDERER=opengl` (PowerShell: `$env:PENUMBRA_UI_RENDERER="opengl"; penumbra`). On a machine with no display at all, use `--terminal` or `--worker`. |
| Worker: "could not join: the connection closed before the controller answered" (exit code 6) | Something closed the connection before the controller said anything: the address or port is not the controller's, a firewall, proxy or relay on the way drops it (on a rented container, check the relay in [Rented GPU containers](#rented-gpu-containers-vastai-runpod-and-similar)), or the controller is ignoring this address after more than 5 wrong codes (wait 10 minutes or restart the controller). |
| Worker: "could not join: the controller did not accept the join code" (exit code 6) | The code differs from the one the controller printed; copy it again (case and dashes do not matter). After more than 5 wrong codes in a minute the controller ignores that address for 10 minutes: wait, or restart the controller. |
| Worker: "the controller rejected this worker: ... runs Penumbra ... (git ...)" (exit code 6) | The two machines run different builds. Run `git pull` and rebuild on both ([Updating](#updating)); the git hashes must match. |
| Worker: "cannot connect to ...: connection refused" or timed out (exit code 7) | Check, in order: the controller is running with `--host` and printed "hosting on port 7420"; the address is one the controller listed; the firewall allows TCP 7420 on the controller (Windows: allow Penumbra when asked, or `New-NetFirewallRule -DisplayName Penumbra -Direction Inbound -Protocol TCP -LocalPort 7420 -Action Allow` in an admin PowerShell; Linux: `sudo ufw allow 7420/tcp`); for internet workers, the router forwards TCP 7420 (the controller logs whether UPnP did it) or both machines are on the same VPN. |
| Controller log: "UPnP: the router's internet address ... is not public" | Carrier-grade NAT or a second router: port forwarding cannot work. Use a VPN such as Tailscale on both machines and join with the controller's VPN address. |
| Controller log: "&lt;worker&gt; cannot take this scene: not enough disk / RAM" | That worker has no room for the scene; the render goes on without it. Free space or use `--cache-dir` on a bigger drive on the worker. |
| Host shows "upload-bound" | The worker renders faster than its uplink can send tiles. Once its results wait more than 2 s, the controller gives it items of 2, 4, then up to 8 chunks, which need up to 8x less upload per sample (logged as "N chunks per item"); past 512 MB waiting it stops asking for work until they are sent. Nothing to fix unless it persists. |
| Controller log: "&lt;worker&gt; sent nothing for 30 s (heartbeat lost)" | The connection to that worker died (network, sleep, crash). Its work goes to others; with `--stay` it reconnects on its own. |
| Out of disk (exit code 4) | The film needs space on the controller (table above). Use `--film-dir` on a bigger drive or host from a machine with more disk. A render that runs out of space mid-way checkpoints first; free space, then continue with `--resume`. |
| `--resume` refuses: "rendered by Penumbra ... (git ...)" | The film was started by another build. Check out that commit and rebuild to finish it, or start the render again. |

## Updating

Controller and workers must run exactly the same version and git commit. On **every** machine:

1. Get the latest code, in the `Penumbra` folder:
   ```
   git pull
   ```
2. Rebuild on Windows:
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\build.ps1
   ```
   or on Linux:
   ```bash
   ./scripts/build.sh
   ```
3. Check the version; every machine must print the same git hash:
   ```
   penumbra --version
   ```

## License

MIT; see [LICENSE](LICENSE).
