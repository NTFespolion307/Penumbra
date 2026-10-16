# Penumbra

Penumbra is a distributed, multi-GPU path tracer for **Windows 10/11, Ubuntu 22.04+ and Debian 12+**. It renders
still images from 120p up to 160K (163840 × 92160, 15.1 gigapixels) from glTF scenes exported from Blender.

One machine is the **controller**: it hosts the render, sends the scene out and collects the finished tiles into
the image. Any number of **workers**, each with one or more GPUs, join with the controller's address and a join
code, render tiles and send them back. A render on one machine is the same thing with no workers, and every
combination of machines, GPUs and backends converges to the same image.

> **Status: milestone M0 (foundation).** This build parses and validates every option, loads config files and
> `.pen.json` scenes, logs, and prints version and diagnostics. It does not render yet: the CPU renderer arrives
> in M1, networking in M4, GPU backends in M5/M6, the UI in M10. Features marked *(planned, Mx)* below are not
> built yet. The roadmap is in [`docs/GUIDE.md`](docs/GUIDE.md).

<!-- A screenshot goes here once the UI exists (M10). -->

**Supported GPUs (planned):** NVIDIA first (RTX 20 series and newer through Vulkan ray tracing, any NVIDIA GPU
through CUDA). AMD RDNA2+ and Intel Arc should work through Vulkan ray tracing but are untested. Integrated
GPUs without ray tracing do not render in v1; the CPU renders instead.

## Contents

1. [Why you build it yourself](#why-you-build-it-yourself)
2. [Prerequisites](#prerequisites)
3. [Build](#build)
4. [Running on one machine](#running-on-one-machine)
5. [Rendering with several machines](#rendering-with-several-machines)
6. [CLI reference](#cli-reference)
7. [Resolutions, backends, memory and formats](#resolutions-backends-memory-and-formats)
8. [Scenes from Blender](#scenes-from-blender)
9. [Troubleshooting](#troubleshooting)
10. [Updating](#updating)

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
| Vulkan SDK | 1.3.280+ | *(planned, M5)* not needed yet |
| CUDA Toolkit | 12.8+ | *(planned, M6)* optional, NVIDIA only; not needed yet |

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
| Vulkan SDK or distro Vulkan packages | 1.3.280+ | *(planned, M5)* not needed yet |
| CUDA Toolkit | 12.8+ | *(planned, M6)* optional, NVIDIA only; not needed yet |

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

## Build

### One command (recommended)

The build scripts check the prerequisites, configure, build, run the tests and a smoke test, and print where
the binary is. The first build also compiles the C++ libraries through vcpkg and takes a few minutes.

**Windows** (any PowerShell window; the script sets up the Visual Studio x64 environment itself):

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
`-Clean` / `--clean` to start from an empty build folder.

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
   ctest --preset windows-msvc-release
   ```

The binary is `build/<preset>/bin/penumbra` (`penumbra.exe` on Windows).

### Build options

| CMake option | Default | Meaning |
| --- | --- | --- |
| `PEN_ENABLE_VULKAN` | `ON` | Vulkan RT backend and UI swapchain *(planned, M5)* |
| `PEN_ENABLE_CUDA` | `ON` if the CUDA toolkit is found | CUDA backend *(planned, M6)* |
| `PEN_ENABLE_OPTIX` | `OFF` | OptiX backend and denoiser *(planned, M13, optional)* |
| `PEN_ENABLE_UI` | `ON` | The user interface *(planned, M10)*; turn off for headless servers |
| `PEN_CUDA_ARCHS` | `75;80;86;89;90;120` | CUDA architectures to compile for |
| `PEN_BUILD_TESTS` | `ON` | Unit, smoke and lint tests |

Pass options at configure time, for example a headless server build without CUDA or UI:

```
cmake --preset linux-gcc-release -DPEN_ENABLE_CUDA=OFF -DPEN_ENABLE_UI=OFF
```

## Running on one machine

| Command | What it does |
| --- | --- |
| `penumbra` | Opens the UI as controller *(planned, M10; M0 shows a notice)* |
| `penumbra --terminal scene.glb -o out.png` | Headless render on this machine's devices *(planned, M1)* |
| `penumbra --terminal scene.glb --res 720p --spp 16 -o preview.png` | Quick preview *(planned, M1)* |
| `penumbra --terminal scene.pen.json --res 4k --spp 1024 -o final.exr` | 4K final with settings from a `.pen.json` *(planned, M1)* |
| `penumbra --terminal --resume final.session.json` | Continue an interrupted render *(planned, M2)* |
| `penumbra --list-devices` | Lists the devices this machine can render with |
| `penumbra --diagnose` | Prints OS, CPU, RAM, devices, folders and free disk. **Paste this into bug reports.** |
| `penumbra --version` | Prints version, git hash and protocol version |

Settings come from four layers, highest priority first: command-line flags, the `render` section of a
`.pen.json`, your user config file, then built-in defaults. The user config file holds personal defaults using
the same keys as the CLI reference (for example `{"tonemap": "filmic", "worker_name": "gpu-server-1"}`):

| OS | User config | Logs |
| --- | --- | --- |
| Windows | `%APPDATA%\Penumbra\config.json` | `%LOCALAPPDATA%\Penumbra\logs` |
| Linux | `~/.config/penumbra/config.json` | `~/.local/state/penumbra/logs` |

The join code is never read from or written to the config file.

## Rendering with several machines

*(planned, M4: everything in this section describes the finished networking; it is not built yet.)*

**Hosting.** In the UI, LAN hosting starts with the app and the Dashboard shows the join code and the address to
give workers; internet hosting is switched on in Settings. Headless, add `--host`:

```
penumbra --terminal --host scene.glb -o out.exr --res 8k
```

The controller prints its addresses and a join code like `K7QD-M2XR-9FHT-4WNB`. The code stays the same across
restarts until you regenerate it.

**Joining.** On each worker machine:

```
penumbra --worker --join 192.168.1.20 --code K7QD-M2XR-9FHT-4WNB
```

Set the code in the `PENUMBRA_CODE` environment variable instead of `--code` to keep it out of shell history.
Add `--stay` on always-on machines: the worker then waits for the next render and reconnects on its own if the
controller restarts. In the UI, use **Join a render**.

**LAN vs internet.** Workers connect *to* the controller, so only the controller needs a reachable port, TCP 7420
(change it with `--port`).

- **LAN:** join with the controller's LAN address (e.g. `192.168.1.20`).
- **Internet:** enable internet hosting; Penumbra tries UPnP to open the port on your router (`--no-upnp` to
  skip). If UPnP is off on the router, forward TCP 7420 to the controller by hand, then join with your public IP.
- **Carrier-grade NAT** (no public IPv4, common on mobile and some fibre ISPs): port forwarding cannot work. Put
  the controller and workers on a VPN such as Tailscale or WireGuard and join with the controller's VPN address.

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
| Devices | `--devices` | all, gpu, or a list like 0,1,cpu | all | `penumbra --devices 0,cpu` | `devices` | Which local devices render (controller or worker) |
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
| Info | `--benchmark` |  |  | `penumbra --benchmark` | CLI only | Run the benchmark suite (from M9) |
| Subcommand | `merge <films...> -o <output>` |  |  | `penumbra merge a.penfilm b.penfilm -o out.exr` | CLI only | Combine partial films into one output image |
<!-- END CLI REFERENCE -->

**Exit codes:** 0 success, 1 bad arguments (also: mode not built yet in this milestone), 2 scene load error,
3 no usable device, 4 out of disk, 5 interrupted (checkpoint written), 6 rejected by controller, 7 connection
lost.

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
Before a render starts Penumbra checks there is room for the film and the output *(planned, M2)*.

### Backends *(planned, M1/M5/M6/M13)*

| `--backend` | Runs on | Role |
| --- | --- | --- |
| `vulkan` | NVIDIA RTX 20+, AMD RDNA2+, Intel Arc | Default on GPUs with ray tracing; uses the RT cores |
| `cuda` | Any NVIDIA GPU | GPUs without RT cores; 2–5× slower than Vulkan RT |
| `cpu` | Any x86-64 or ARM64 CPU | Fallback and reference; joins GPUs in hybrid mode |
| `optix` | NVIDIA RTX | Optional, only if it beats Vulkan RT |

`--backend auto` (default) picks Vulkan RT on every GPU with ray-tracing support, CUDA on NVIDIA GPUs without it,
and the CPU only when no GPU qualifies or `--devices` lists `cpu`. Every choice is printed in the log.

### Multi-GPU and memory tiers *(planned, M7/M8)*

A worker uses all its GPUs at once; each GPU pulls its own work, so fast GPUs do more of the frame. Each GPU
picks a memory tier for the scene: **0** everything in VRAM; **1** textures in RAM (1.2–2× slower); **2**
textures downsized (disable with `--no-texture-downsize`); **3** GPU skipped. Override the budget with
`--vram-limit` and `--ram-limit`. A GPU that runs out of memory never crashes the render.

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

*(planned, M1: loading scenes is not built yet; the `.pen.json` format below is already parsed and validated.)*

**Export:** in Blender, File → Export → glTF 2.0. Choose **glTF Binary (.glb)**, and under Include tick
**Cameras** and **Punctual Lights**. Under Data → Mesh keep **Apply Modifiers** on. Materials should use the
Principled BSDF; supported extensions include transmission, IOR, clearcoat, sheen, specular, emissive strength,
anisotropy, texture transform, punctual lights and GPU instancing.

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

## Troubleshooting

Start with `penumbra --diagnose` and include its output in any bug report.

| Problem | What to do |
| --- | --- |
| Build script: "vcpkg not found" | Follow [Prerequisites](#prerequisites) steps for vcpkg, then open a new terminal so `VCPKG_ROOT` is set. |
| Build script: "Visual Studio ... without the C++ x64 tools" | In the Visual Studio Installer, add the "Desktop development with C++" workload. |
| Linux: "CMake 3.26 or newer is required" | Install the official CMake as in [Prerequisites](#prerequisites); check `which cmake` points to `/opt/cmake/bin/cmake`. |
| No Vulkan ray tracing *(M5)* | Update the GPU driver; `penumbra --diagnose` shows whether each GPU reports ray-tracing support. GPUs without it use CUDA (NVIDIA) or are skipped. |
| CUDA not found *(M6)* | Install CUDA 12.8+ and rebuild; without it Penumbra still builds with Vulkan and CPU. |
| Worker rejected *(M4)* | `version_mismatch`: update and rebuild both machines ([Updating](#updating)). Wrong code: copy it again from the controller; codes are case-insensitive and dashes are optional. |
| Worker can't connect *(M4)* | Check the address, that the controller is hosting, the firewall rule for TCP 7420, port forwarding or UPnP for internet workers, or use a VPN behind carrier-grade NAT. |
| Host shows "upload-bound" *(M4)* | The worker renders faster than its uplink can send tiles; it pauses itself and catches up. Nothing to fix unless it persists. |
| Out of disk *(M2)* | The film needs space on the controller (table above). Use `--film-dir` on a bigger drive or host from a machine with more disk. The render pauses with a checkpoint and resumes with `--resume`. |

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
