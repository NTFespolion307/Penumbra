# Penumbra — instructions for Claude Code

Penumbra is a distributed, multi-GPU path tracer for Windows 10/11, Ubuntu 22.04+ and Debian 12+.
One CONTROLLER hosts a render and sends the scene; one or more WORKER hosts (each with 1+ GPUs) join with
address + join code, render tiles and send them back. Stills from 120p to 160K (15.1 gigapixels).
One binary: no args = UI (controller, with "Join a render"); `--terminal` = headless controller; `--worker` = headless worker.

## Source of truth

- `docs/GUIDE.md` is the full planning guide (exported from the planning doc). Read it at the start of EVERY session; it wins over assumptions.
- `docs/DECISIONS.md`: every decision made during the build (create in M0). Read every session.
- `docs/BENCHMARKS.md`: measured performance (from M5). Read the last entry every session.
- `docs/PROTOCOL.md`: wire protocol, kept in sync with `net/schema/*.fbs`.
- Open decisions in the guide: build with the stated default until the user answers.
- The Markdown export drops the guide's three diagrams; their text versions are below.

## Architecture (text version of the guide's diagram)

```
+---------------- CONTROLLER HOST (one per render) ----------------+
| UI or CLI        | Session (TCP 7420,   | Scene packager         |
|                  | TLS 1.3 PSK, UPnP,   | (.penpkg + BLAKE3,     |
|                  | worker list, kick)   |  sent once, cached)    |
| Scheduler        | Film (running mean + | Output (denoise ->     |
| (tile x sample-  | sample count; RAM or | tonemap -> encode,     |
|  range leases)   | .penfilm, checkpoint)|  streamed writers)     |
| Local worker: controller's own GPUs/CPU, same WorkerLink, no net |
+------------------------------------------------------------------+
      | scene package + leases               ^ tile results
      v                                      |
+------------- WORKER HOST (xN, each with 1 or more GPUs) ---------+
| Network client (TLS-PSK, heartbeats, uploads, backpressure)      |
| Scene cache (verify hash, LRU on disk) | Device manager (1 slot/GPU)|
| Backends: Vulkan RT (default) | CUDA compute | CPU/Embree (ref)  |
|           OptiX (optional, late)                                 |
| Shared shading code in Slang -> SPIR-V, CUDA C++, CPU C++        |
+------------------------------------------------------------------+
```

Rules: pixels exist only in the controller's film; workers never talk to each other; any work item can be
rendered by any device on any host (deterministic sampling); solo renders use the same path (local worker).

## Network topology (text version)

Workers DIAL OUT to the controller, so only the controller needs a reachable port (TCP 7420):
LAN address, port-forwarded public IP (UPnP tried), or a VPN address (Tailscale/WireGuard) behind carrier-grade NAT.

```
penumbra --worker --join 192.168.1.20 --code K7QD-M2XR-9FHT-4WNB         # LAN worker
penumbra --worker --join 203.0.113.5 --code K7QD-M2XR-9FHT-4WNB --stay   # always-on internet worker
penumbra --terminal --host scene.glb -o out.exr --res 160k                # headless controller
```

Session vs render: hosting is a long-lived SESSION. Workers join and stay connected while idle (the UI Dashboard shows
hosts, GPU count, cluster CPU/RAM before any render), get the scene when a render starts, and remain for the next render.
In the UI, LAN hosting starts with the app; internet hosting is opt-in. The join code persists across controller restarts
(user data folder, never the config file) until Regenerate. Every host sends `Stats` every 2 s, also when idle:
host CPU %, RAM, per-GPU usage (NVML on NVIDIA, else Penumbra busy %), VRAM, temperature.

Key specs (details in GUIDE.md): join code = 16 chars Crockford base32 (~80 bits), used to derive a TLS 1.3
external PSK (HKDF-SHA256, label `penumbra-join-v1`), never sent. Frames: `PNBR | proto ver | type | flags | len | FlatBuffers`,
max 16 MB, bulk in 4 MB chunks interleaved with control frames. Exact version + git hash match required.
Heartbeat 5 s, peer lost after 30 s, leases requeued. Tiles: float16 (float32 if out of range) + sample count, zstd.

## Milestones (text version of the roadmap)

Do them in order. Done = gate passes on BOTH Windows and Linux AND a fresh clone builds from README.md alone.
M5 and M6 may be swapped.

| # | Milestone | Scope | Gate |
|---|---|---|---|
| M0 | Foundation | CMake presets, vcpkg, CLI parsing, mode dispatch, logging, config, build scripts, README | Fresh clone builds via README on both OSes; `--help` works; no console from Explorer |
| M1 | CPU reference renderer | glTF + Embree, path tracer NEE + MIS, principled BSDF, HDRI, Sobol, PNG/EXR | Furnace test passes; same image for any thread count; Cornell box looks right |
| M2 | Tiled film + gigapixel output | Tiles, `.penfilm`, film merge, streamed writers, checkpoint + `--resume`, all 17 presets | 20K on laptop and 160K on server open; kill + `--resume` is identical |
| M3 | Scheduler + local worker | `WorkerLink`, in-process worker, work items, leases, pass order, sample-range splitting | N local CPU slots match 1 slot; leases requeue on simulated device failure |
| M4 | Network controller + workers | Protocol, TLS-PSK join codes, scene packages + cache, heartbeats, UPnP, `--stay`, resume | Loopback, LAN and internet workers match a solo render; all fault tests pass |
| M5 | Vulkan RT backend | BLAS/TLAS, RT pipeline, Slang shaders, one GPU; decide Slang CPU target vs C++ headers | Matches CPU golden images, relMSE < 1e-3 at 4,096 spp |
| M6 | CUDA compute backend | tinybvh CWBVH, Slang to CUDA kernels, sm_75 to sm_120 | Matches CPU golden images; `--backend cuda` runs on any NVIDIA GPU |
| M7 | Multi-GPU workers | One slot per GPU, mixed backends per host, tail duplication, device dropout, hybrid CPU | Multi-GPU worker matches 1 GPU; a removed GPU never stops the render |
| M8 | Memory budget + fallback | Tier 0-3 ladder, pinned RAM textures, texture downsizing, host-AS probe, placement log | `--vram-limit` tests reach every tier on the 8 GB RTX 5060; zero OOM crashes |
| M9 | Performance + benchmarks | Profiling, megakernel vs wavefront, `--benchmark`, network throughput | Measured speeds replace estimates; LAN workers keep GPUs > 90% busy |
| M10 | UI | Dashboard, Hosts + host detail, Render and Viewport pages, Settings, Join a render (see GUIDE.md "UI") | Host, join and finish a 3-machine render with clicks only |
| M11 | Denoise, tonemap, AOVs | OIDN per tile with 32 px border, AgX/Filmic/ACES, AOVs, metadata | No seams on a 40K render; AgX close to Blender |
| M12 | Release + README | Build scripts polished, full README audit incl. networking, batch queue, release archives | Fresh clone builds on Win 11, Ubuntu 22.04, Debian 12 from README alone |
| M13 | OptiX (optional) | OptiX pipeline + denoiser behind `PEN_ENABLE_OPTIX` | Kept only if it beats Vulkan RT on the benchmark suite |

M4 order: loopback protocol (controller + 3 worker processes) -> TLS-PSK + join codes -> scene packages + cache + resume
-> heartbeats, leases, every fault-table row -> `--stay` + controller `--resume` -> UPnP + internet test. Commit and push each step.

## GitHub: all work is saved to github.com/NTFespolion307/Penumbra

- Remote `https://github.com/NTFespolion307/Penumbra.git`, branch `main`.
- First session: commit `CLAUDE.md` and `docs/GUIDE.md`, push.
- Commit at every green test run (message starts with the milestone, e.g. `M4: lease requeue`).
- `git push origin main` at the end of EVERY session and after EVERY passed gate. Never end with unpushed commits. If a push fails, stop and tell the user.
- Renders and scenes over 50 MB: Git LFS or keep out of the repo.

## README.md is a required deliverable (from M0, updated in the same commit as any change it describes)

Windows SmartScreen blocks unsigned downloaded exes, so users build locally; a locally built exe runs without a warning.
Build scripts: `scripts/build.ps1` / `scripts/build.sh` check prerequisites, configure, build, run a CPU smoke test, print the binary path.

README must contain: (1) what Penumbra is + controller/worker idea; (2) why build yourself (SmartScreen);
(3) prerequisites per OS with exact versions and copy-paste install commands; (4) build: scripts first, then manual CMake presets, with/without CUDA/UI;
(5) running solo: UI, `--terminal` examples, `--resume`, `--list-devices`, `--diagnose`;
(6) rendering with several machines: hosting (UI and `--terminal --host`), joining (`--worker --join --code`), `--stay`, LAN vs internet,
port forwarding/UPnP, VPN for CGNAT, Windows Firewall and `ufw allow 7420/tcp`, a full 160K example across three machines;
(7) full CLI reference generated from the same source as `--help`; (8) presets, backends, multi-GPU, memory tiers, formats and limits, controller disk needs;
(9) Blender `.glb` export + `.pen.json`; (10) troubleshooting (no Vulkan RT, CUDA not found, worker rejected, can't connect, upload-bound, out of disk);
(11) updating: `git pull` + rebuild on EVERY machine (controller and workers must run the same version).

## Decided targets

- Every host may run Windows OR Linux; never assume which. Controller and workers mix freely.
- NVIDIA first: tune and validate every GPU milestone on NVIDIA. AMD RDNA2+ / Intel Arc work via Vulkan RT but are untested.
- Integrated GPUs without RT (the laptop's Intel GPU) do not render in v1: `--backend auto` skips them; they still drive the UI.
- Scale: design and test for 5+ multi-GPU servers (scale test: 64 simulated workers via a test-only `--backend null`).
- UI: match the approved mockups in `docs/ui-mockups/` (see its README) and GUIDE.md "UI".

## Machines and GPU testing

| Machine | Hardware | Used for |
|---|---|---|
| Dev laptop (where you run) | Intel i7 11th gen, 32 GB RAM, Intel integrated GPU WITHOUT ray tracing (UI only, never renders in v1) | All coding; CPU, unit and network tests (several worker processes); compiling GPU code; renders up to 20K |
| Gaming PC | RTX 5060, 8 GB, Blackwell sm_120, hardware RT | Single-GPU Vulkan RT + CUDA, Windows GPU tests, low-VRAM fallback, LAN worker |
| GPU servers (up to 5) | Mostly NVIDIA, mixed models | Multi-GPU, 60K-160K renders, internet-worker tests, benchmarks |

- `PEN_CUDA_ARCHS` default `75;80;86;89;90;120`; add older archs if the server has them.
- Never mark M5-M9 as passed without results from the gaming PC or the GPU server; reports name the machine and GPU.
- Preferred: both GPU machines are self-hosted GitHub Actions runners (`gpu-windows`, `gpu-linux`) running `ctest -L gpu` on pushes to `main` only, never on pull requests from forks.
- Fallback: `scripts/test-gpu.ps1` / `scripts/test-gpu.sh`; ask the user to run them and paste the summary.

## Working rules

1. Read this file, `docs/GUIDE.md`, `docs/DECISIONS.md` and the last `docs/BENCHMARKS.md` entry at the start of every session.
2. One milestone at a time. Post a short plan and wait for the user's OK before coding it. When the gate passes, STOP and report.
3. Correctness first, then speed. A change that alters golden images is a bug.
4. Everything goes through controller -> scheduler -> WorkerLink -> film, including solo renders. No shortcuts.
5. Strict layering: `core/` has no GPU or socket headers; only `backends/` includes Vulkan/CUDA/OptiX/Embree; only `net/`, `controller/hosting`, `worker/client` touch sockets.
6. Every new flag: `RenderSettings`, `--help`, `.pen.json` schema, config file, UI and README in the same commit. Every protocol change: `net/schema`, protocol version bump, `docs/PROTOCOL.md`.
7. Log every automatic decision (backend, memory tier, requeue, kick, fallback) at `info`. No silent fallbacks.
8. When reality differs from the guide, test it, pick what works, write it to `docs/DECISIONS.md`.
9. Ask before adding an unlisted dependency, renaming a CLI flag, or changing the default port 7420.
10. Test on both OSes before closing a milestone (MSVC preset on Windows; GCC preset in a clean Ubuntu 22.04 container).
11. Push to GitHub at the end of every session and every passed gate.
12. A milestone is not done until a fresh clone builds on Windows and Linux by following README.md alone.
