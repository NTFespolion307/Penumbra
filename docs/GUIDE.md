# Penumbra — Raytracer Planning & Build Guide

Oct 16, 2026 · @NTF307

## Project identity

**Penumbra** is a distributed, multi-GPU path tracer for Windows and Linux. One **controller** hosts a render and sends the scene out; one or more **worker** hosts, each with one or more GPUs, join with an address and a join code, render tiles and send them back. Together they render stills from 120p to 160K (15.1 gigapixels). The binary is `penumbra` (`penumbra.exe` on Windows); the same binary is controller or worker.

This guide is the single source of truth for the implementing agents (Claude Code). It is ordered from the outside in: what the system must do, how controller and workers cooperate, how a single device renders, then the app, the build and the process. Where a choice is still open it is listed in **Open decisions** with a default; build with the default until the user answers.

Code lives at **github.com/NTFespolion307/Penumbra**. Alternative names considered: Refrakt, Photonforge, Umbra.

## Glossary

These terms are used with exactly these meanings throughout the guide and the code.

| Term | Meaning |
| --- | --- |
| Controller | The one machine that runs a render: holds the scene, hands out work, collects tiles into the film, writes the output. |
| Worker (host) | Any other machine that joins a render and contributes its devices. |
| Local worker | The controller's own devices, connected to the scheduler in-process instead of over the network. |
| Device / slot | One GPU (or the CPU) on a host. Each device is a slot that asks for and renders work on its own. |
| Session | The controller's hosting period: its join code and connected workers. Many renders can run one after another in one session; each render's resume file is `<name>.session.json`. |
| Join code | 16-character code (`XXXX-XXXX-XXXX-XXXX`) a worker needs to join; also the encryption key. |
| Scene package | The scene, textures and HDRI bundled into one hashed `.penpkg` file that the controller sends to workers. |
| Tile | A rectangle of the image, 256×256 px by default. |
| Work item | A tile plus a range of samples to render for it (e.g. samples 0–63). |
| Lease | A work item handed to one device, valid while its host keeps sending heartbeats. |
| Film | The controller's accumulation buffer: running average colour and sample count per pixel, in RAM or a `.penfilm` file. |
| Checkpoint | A periodic flush of film and session to disk so a render can resume after a crash. |
| spp | Samples per pixel: how many light paths are averaged per pixel; more means less noise. |
| Bounce | One reflection or refraction of a light path; `--max-bounces` caps them. |
| Backend | The engine that traces rays on a device: Vulkan RT, CUDA, CPU (Embree), or optional OptiX. |
| Integrator | The light-transport algorithm (default: path tracing with next-event estimation and MIS). |
| BVH | Bounding volume hierarchy, the tree that makes finding ray hits fast; "acceleration structure" in Vulkan. |
| Memory tier | Where a device keeps scene data: 0 all in VRAM, 1 textures in RAM, 2 smaller textures, 3 device skipped. |
| AOV | An extra output pass such as albedo, normal or depth. |
| Golden image | A reference render that every backend and every distribution of work must match. |
| Gate | The test a milestone must pass before the next milestone starts. |

## Goals, requirements and non-goals

Penumbra v1 renders one still image at a time, across every device the user can reach: the controller's own devices plus any number of worker hosts. Every combination of machines, backends and GPUs must converge to the same image for the same scene and seed.

| Requirement | What it means for the build |
| --- | --- |
| Controller + workers (primary mode) | Every render has exactly one controller. Zero or more worker hosts join over LAN or internet with address + join code. The controller sends the scene; workers send finished tiles back. A render with no workers is the same code path with only local devices. |
| 1 or more GPUs per worker | A worker uses all of its GPUs (and optionally its CPU) at once; each device pulls its own work. |
| Workers join any time | Before or during a render; a worker that leaves or crashes never stops or corrupts the render. |
| Windows + Linux | Windows 10/11 x64; Ubuntu 22.04+ and Debian 12+. Controller and workers may run different OSes in one render. |
| UI and terminal modes | No args opens the UI as controller; Join a render on its start screen makes it a worker; `--terminal` runs a headless controller; `--worker` runs a headless worker. Flags identical on both OSes. |
| Multiple backends | Hardware RT (Vulkan RT), GPU compute on CUDA cores (CUDA), CPU (Embree). Optional OptiX later. |
| VRAM first, RAM fallback | A per-device memory budget decides placement; no out-of-memory crash, ever. |
| Custom spp and bounces | spp from 1 to 2^32-1; max bounces from 0 to 1024. |
| 120p to 160K | 17 presets in 16:9 plus custom WxH; outputs up to 15.1 gigapixels. |
| Build from source | Windows SmartScreen blocks unknown downloaded exes, so users build locally; README.md and build scripts are first-class deliverables. |

**Non-goals for v1:** animation and motion blur; voxel volumes (OpenVDB clouds and smoke) and subsurface scattering (an atmosphere and physical sky are planned as M14, D-105); spectral rendering; macOS; AMD HIP and Intel oneAPI compute backends (AMD/Intel GPUs are covered through Vulkan RT); a scene editor; several renders at once on one controller; worker-to-worker traffic; untrusted public worker pools. **Trust model:** workers are machines the user controls. The join code keeps strangers out, but Penumbra does not try to detect a malicious worker that sends wrong pixels.

## System architecture

The controller owns everything that must exist once (scene, schedule, film, output); workers own devices and only turn work items into tiles. The controller's own devices are a local worker behind the same `WorkerLink` interface, so solo and distributed renders share one code path.

&#91;embedded content: Penumbra architecture · controller host and worker hosts\]

Three rules keep the system simple. Pixels exist in exactly one place, the controller's film. Workers never talk to each other. Any work item can be rendered by any device on any host, because sampling is deterministic (see Integrators), so the scheduler never needs to care where a tile came from.

## Render lifecycle

A render goes through ten steps; steps 4 to 6 can happen for a new worker at any time, including in the middle of step 7.

1. **Prepare (controller).** Load scene and settings, validate them (format/size limits, free disk for film and output), build the scene package and its hash, open or resume the film.
2. **Host (controller).** Start listening on TCP 7420, generate the join code, list LAN/VPN/public addresses, and try UPnP if internet hosting is enabled. In the UI, LAN hosting starts with the app so the Dashboard shows hosts before any render (internet hosting is opt-in); headless it is on with `--host`.
3. **Local devices start.** The controller's own devices register as an in-process worker and begin rendering immediately, unless `--no-local`.
4. **Join (worker).** The worker connects, completes the TLS 1.3 PSK handshake keyed by the join code, sends `Hello` (versions, name, OS, devices); the controller replies `Welcome` or `Reject` with a reason.
5. **Scene transfer.** When a render starts, or when a worker joins mid-render, the controller sends `SceneOffer`; the worker answers from its cache or downloads the missing chunks, then verifies the hash.
6. **Ready.** The worker loads the scene once, builds acceleration structures per device, picks each device's memory tier, and sends `WorkerReady` with the result per device.
7. **Render.** Each device slot requests work; the scheduler grants leases (tile × sample range); workers stream back `TileResult`s; the controller validates, merges them into the film, acknowledges, updates preview and progress, and checkpoints every 10 minutes.
8. **Complete.** When every work item is merged, the controller sends `RenderComplete`. Workers stay connected and idle for the next render, except headless workers started without `--stay`, which disconnect and exit 0.
9. **Output (controller only).** Denoise, tonemap and encode tile by tile from the film into the output files.
10. **Close.** The render's session file is deleted and a summary (time, samples, tiles per worker and device) is logged and shown.

## Controller and workers

One controller per render; any number of workers join it by address and join code. Workers only dial out, so only the controller needs a reachable port.

&#91;embedded content: Network topology · one controller, any number of workers\]

**Controller.** The UI by default, or headless with `--terminal --host`. It owns the session (code, listener, worker list), scheduler, film and output, and renders on its own devices unless `--no-local`. On a controller with no ray-tracing GPU (the dev laptop), the local CPU worker leaves 2 cores free so the controller stays responsive while writing the film.

**Worker.** Headless with `penumbra --worker --join <address>[:port] --code XXXX-XXXX-XXXX-XXXX`, or the Join a render screen in the UI. It uses all its GPUs by default (CPU only if no GPU or `--devices` names it). `--stay` keeps it connected after a render and reconnects on its own if the controller restarts, which suits an always-on GPU server.

**Session versus render.** Hosting is a long-lived session, not tied to one render: workers join the session, stay connected while idle (so the Dashboard shows hosts, GPUs, CPU and RAM before anything renders), receive the scene when a render starts, and remain for the next one. A render is a job inside the session; finishing or stopping it never disconnects anyone.

**Join code.** 16 characters of Crockford base32 (0–9, A–Z without I, L, O, U), shown as 4-4-4-4, about 80 bits of entropy, typed case-insensitively with dashes optional. The code persists across controller restarts (stored in the user data folder, never in the config file) so always-on workers can reconnect; Regenerate revokes it immediately and disconnects every worker using it. `--code` sets a fixed code for scripted setups. Optional `--approve-workers` asks the controller user to accept each new worker. More than 5 failed handshakes from one IP in a minute block that IP for 10 minutes.

**Security.** The code is never sent. Both sides derive the TLS 1.3 external pre-shared key from it (HKDF-SHA256, label `penumbra-join-v1`), so the connection is encrypted and a wrong code cannot complete the handshake. Implement with mbedTLS 3.x (TLS 1.3 PSK); if PSK support proves inadequate in M4, use Noise `NNpsk0` (noise-c) instead and record it in `docs/DECISIONS.md`. Workers accept only scene data and work items; the controller accepts only status and tiles. Nothing received is ever executed.

**Addresses, NAT and firewall.** The Network panel lists every LAN address, any VPN address (Tailscale/WireGuard interfaces), and, after an opt-in lookup, the public IP. When the user allows internet workers, the controller tries a UPnP port mapping (miniupnpc) and shows whether it worked; otherwise the user forwards TCP 7420. Behind carrier-grade NAT there is no forwardable public IP: use a VPN and join with its address. Windows Defender Firewall prompts the first time the controller listens; on Linux the README shows the `ufw allow 7420/tcp` command.

**Controller disk.** The film lives only on the controller: about 242 GB free for a 160K render. Use `--film-dir` to put it on a big drive, or host 160K renders from the GPU server.

## Wire protocol

One TLS-encrypted TCP connection per worker carries every message as a length-prefixed frame with a FlatBuffers payload; bulk data is chunked so control messages never wait behind it.

**Transport.** TCP, default port 7420, standalone Asio for async I/O on both OSes, TCP keepalive on, Nagle off. One connection per worker host (not per GPU); device ids inside messages address individual GPUs.

**Frame.** `magic "PNBR" (4 B) | protocol version u16 | message type u16 | flags u16 (bit 0 = zstd) | payload length u32 | payload`. Maximum payload 16 MB; larger data (scene, big tiles) is split into chunks of up to 4 MB. The sender interleaves queues so a control frame is never behind more than one chunk.

**Schemas** live in `net/schema/*.fbs`. FlatBuffers keeps them versionable and readable from Go, so the user's cluster software can speak the protocol later. **Versioning:** the protocol version must match exactly, and so must the Penumbra version and git hash, because identical shading code is what makes tiles from different hosts interchangeable.

| Message | Direction | Contents and purpose |
| --- | --- | --- |
| `Hello` | W → C | Protocol + app version and git hash, worker name, OS, CPU, RAM, free cache disk, device list (name, backends, VRAM, RT support). |
| `Welcome` | C → W | Session id, worker id, controller name, heartbeat interval. |
| `Reject` | C → W | Reason: `version_mismatch`, `not_approved`, `session_full`, `session_closed`, `kicked`; human-readable text. |
| `SceneOffer` | C → W | Scene hash, package size, chunk count, full `RenderSettings`, settings hash, film size. |
| `SceneHave` | W → C | `cached`, or `need` with the first missing chunk index (resumable download). |
| `SceneChunk` | C → W | Chunk index, bytes (zstd). |
| `WorkerReady` | W → C | Per device: chosen backend, memory tier, BVH build time; or `scene_rejected` with reason (too big for RAM, disk). |
| `RequestWork` | W → C | Device id, number of items wanted. |
| `Lease` | C → W | Lease id, device id, tile rectangle, sample range, settings hash. |
| `TileResult` | W → C | Lease id, device id, encoding, pixel data (chunked), render time, BLAKE3 checksum. |
| `TileAck` | C → W | Lease id, `accepted` / `duplicate` / `rejected` + reason. |
| `Stats` | W → C | Every 2 s, also when idle: host CPU %, RAM used/total; per device: usage % (NVML, else Penumbra busy %), VRAM used/total, temperature if available, tiles/s, samples/s. |
| `Heartbeat` | both | Every 5 s; 30 s of silence means the peer is gone. |
| `Cancel` | C → W | Lease ids, or all (pause, stop, settings change). |
| `Pause` / `Resume` | C → W | Stop requesting new work / continue. |
| `RenderComplete` | C → W | The render is finished; workers stay idle in the session for the next `SceneOffer` (headless workers without `--stay` exit). |
| `Log` | W → C | Worker warnings and errors, shown in the controller UI and log. |
| `Kick` / `Bye` | C → W / both | Forced or polite disconnect with reason. |
| `Release` | W → C | Lease ids handed back (device failed, worker leaving, backpressure); the items requeue immediately. |

**Tile encoding.** Mean RGB as float16 when every value in the tile fits (|v| ≤ 65,000), otherwise float32, flagged per tile; plus a single sample count when uniform across the tile (the normal case) or a per-pixel uint32 array. Then zstd level 3. NaN or Inf values are a bug signal: the worker replaces them with 0, counts them, and reports the count in the result.

## Scene packages and distribution

The controller turns the scene into one self-contained, content-hashed package; each worker downloads it once and keeps it in a cache, so re-rendering the same scene costs no transfer.

**Package format (`.penpkg`).** A simple archive: a `manifest.json` (Penumbra version, file list with sizes and BLAKE3 hashes, entry scene, `RenderSettings`) followed by the files: the `.glb`/`.gltf` and its buffers, every texture, the HDRI. External paths are rewritten to package-relative paths; files outside the scene folder are copied in. The package hash is the BLAKE3 of the manifest. Packaging also runs for solo renders, because it is the validation step: a missing texture fails here, before any rendering.

**Transfer.** `SceneOffer` → `SceneHave` → `SceneChunk`s of 4 MB, zstd-compressed, verified per chunk and as a whole. An interrupted download resumes from the first missing chunk. The controller sends to several workers at once but caps total upload bandwidth (`--upload-limit`, default unlimited) so tile acks are never starved.

**Upload bottleneck.** When many remote workers start at once, every byte of the scene leaves through the controller's uplink: 6 workers × 1.2 GB = 7.2 GB, which takes most of an hour on a typical home upload. v1 handles this with the worker cache, scene prefetch, and the option to host big remote renders from a server instead of the laptop; peer-assisted distribution (workers on the same LAN sharing chunks) is a post-v1 feature.

**Worker cache.** `%LOCALAPPDATA%\Penumbra\cache\scenes` on Windows, `~/.cache/penumbra/scenes` on Linux, or `--cache-dir`. Least-recently-used eviction above `--cache-size` (default 20 GB). Before downloading, the worker checks free disk and RAM against the manifest and answers `scene_rejected` with a clear reason if the scene cannot fit; the controller shows it next to that worker.

**Settings changes.** Changing anything that affects pixels (camera, materials, resolution, spp, bounces, seed) during a render is not allowed; the UI asks to stop and start a new render. Changing output-only settings (tonemap, exposure, denoise, format) is always allowed, because they apply after the film.

## Scheduling, leases and tiles

The scheduler on the controller splits the frame into work items and leases them to device slots on any host; whichever device is free asks for work, so fast GPUs automatically do more of the frame.

**Ready before work.** A worker never receives a lease before it has sent `WorkerReady`: the full scene package downloaded and hash-verified, the scene loaded, and an acceleration structure built on each device. When the user clicks Start render, the controller's own devices begin at once and every connected worker starts its download in parallel; each worker joins the pool the moment it is ready, without waiting for the others. A worker that joins mid-render goes through exactly the same steps (download, verify, load, build, ready) and then pulls work from the remaining queue. A worker with the scene already cached skips the download.

**Work item.** A tile (default 256×256 px, `--tile-size`) plus a sample range. Frames below 1080p use a single full-frame tile and split by sample range instead, so many devices can share a small image. The sample range per item is tuned so one item takes 2 to 10 seconds on the device that receives it, using that device's measured speed.

**Pass order.** With a RAM film, items go breadth-first: every tile gets its first chunk of samples before any tile gets its second, so the preview refines evenly. With a disk film (large frames), items go depth-first: each tile is finished in one or few items to avoid rewriting it on disk. Tiles are issued in a spiral from the image centre so the most-watched area finishes first.

**Leases.** Each slot holds 2 items in flight (3 when the round trip to its host exceeds 50 ms) so upload, render and download overlap. A lease stays valid while its worker heartbeats. All leases of a worker silent for 30 s return to the queue. A single lease older than 10× the median item time is also reissued, which catches a hung GPU on a host that still heartbeats.

**Results.** The controller checks the lease id, rectangle, sample count and checksum, merges the tile into the film as a sample-weighted mean, writes it (RAM or `.penfilm`), and sends `TileAck`. A result for an item already merged is acknowledged as `duplicate` and dropped. Three rejected results from one worker get it kicked with a logged reason.

**End of render.** When the queue is empty but items are still out, idle fast devices duplicate the oldest outstanding items; the first valid result wins. This stops the last minutes of a big render from waiting on the slowest host.

**Accounting.** Every merged tile records which worker and device produced it. The UI can colour tiles by device, and the final summary lists tiles, samples and time per device.

## Inside a worker: devices and multi-GPU

A worker is a network client, a scene cache and a device manager; each GPU is an independent slot with its own backend instance, its own full scene copy and its own requests for work.

**Device manager.** Enumerates devices at startup, applies `--devices`, and picks a backend per device with `--backend auto` (see Rendering backends). Each GPU gets its own Vulkan logical device or CUDA context; no Vulkan device groups, NVLink or peer-to-peer, so any mix of GPUs works together, including different vendors in one host.

**Per-device flow.** Each slot runs on its own thread: request items → render into a device-local tile buffer → read back → hand to the uploader. Mixed backends in one host are fine (e.g. Vulkan on an RTX card and CUDA on an older GTX card).

**Memory.** Each device runs the memory budget ladder independently (see Memory management). A device that cannot hold the scene at any tier is reported as skipped in `WorkerReady`; the worker still joins with its other devices. If no device qualifies, the worker answers `scene_rejected`.

**Uploader and backpressure.** One sender thread compresses and sends results. If unsent results exceed 512 MB (slow uplink), slots stop requesting new work until the queue drains, so a GPU never renders tiles that cannot be delivered. The worker reports this state in `Stats`, and the UI shows the host as "upload-bound".

**Device failure.** A device that errors (driver reset, out of memory, lost device) is removed; its leases go back to the controller in a `Release` message, and the worker continues with its remaining devices. A worker with no devices left sends `Bye` with the reason.

## Fault tolerance and resume

No single failure of a worker, device or connection may stop a render or corrupt the film; the controller is the only single point of failure, and it can always be resumed from disk.

| Failure | Detected by | What happens |
| --- | --- | --- |
| GPU fails on a worker | Worker | Device removed, its leases released, worker continues on other devices. |
| Worker crashes or network drops | 30 s without heartbeat | All its leases requeued; it may rejoin with the same code and resumes from its scene cache. |
| GPU hangs, host still alive | Lease older than 10× median item time | Item reissued elsewhere; first valid result wins. |
| Corrupt or invalid tile | Checksum, size or sample count check | `TileAck rejected`, item requeued; 3 rejects → worker kicked. |
| Wrong code or old version | Handshake / `Hello` | Connection refused with a clear message; repeated failures block the IP. |
| Scene too big for a worker | Worker before download | `scene_rejected`, shown in the UI; render continues without it. |
| Controller disk full | Film write fails | Render paused, workers told to `Pause`, UI alert; headless exits with code 4 after a checkpoint. |
| Controller crash or power loss | On restart | `penumbra --resume <session.json>` reopens film and session with the same code; `--stay` workers reconnect on their own. |
| User presses Ctrl+C / Stop | Controller | Checkpoint written, current image saved, workers told `RenderComplete`; resumable later. |

**Session file.** Next to the film the controller keeps `<name>.session.json`: scene path and hash, settings, join code, film path, and progress. It is written atomically (write to a temp file, then rename) at every checkpoint, so a crash never leaves a half-written file.

**Film durability.** Tiles are appended to the `.penfilm` as they are merged; the tile index is fsynced at every checkpoint (default 10 minutes, `--checkpoint`). After a crash, at most the tiles since the last checkpoint are rendered again.

## Rendering backends

**Recommendation: Vulkan ray tracing is the default and fastest portable backend.** It uses the GPU's RT cores through `VK_KHR_ray_tracing_pipeline` and `VK_KHR_acceleration_structure`, runs on NVIDIA, AMD and Intel, and works identically on Windows and Linux. On NVIDIA RTX hardware, OptiX can be a few percent faster, but it is NVIDIA-only and duplicates the Vulkan path, so it is deferred to an optional late milestone.

| Backend | `--backend` | Runs on | Traversal | Expected speed (approx.) | Role |
| --- | --- | --- | --- | --- | --- |
| Vulkan RT | `vulkan` | NVIDIA RTX 20+, AMD RDNA2+, Intel Arc | Hardware RT cores, driver-built BVH | Fastest; baseline 1x | Default |
| CUDA compute | `cuda` | NVIDIA GPUs, incl. non-RTX | Software BVH on CUDA cores (tinybvh CWBVH) | 2–5x slower than hardware RT | Non-RTX NVIDIA, cross-check, RAM-resident scenes |
| CPU | `cpu` | Any x86-64 / ARM64 CPU | Embree 4 | 20–100x slower than one high-end GPU | Fallback, reference, joins GPUs in hybrid mode |
| OptiX (optional) | `optix` | NVIDIA RTX | Hardware RT cores | \~1x, sometimes slightly faster | Late milestone, only if benchmarks justify it |

Speed figures are planning estimates; milestone M9 replaces them with measured numbers on the user's hardware.

**Shared shading code.** All path-tracing logic (BSDFs, light sampling, integrator loop) is written once in **Slang** (`shaders/`). Slang compiles to SPIR-V for Vulkan and to CUDA C++ for the CUDA backend. The CPU backend compiles the same Slang modules to C++ through Slang's CPU target; if that target proves too immature, fall back to a shared header-only C++ subset (`core/shading/*.h`) that both nvcc and the host compiler build, and mirror it in Slang for Vulkan. Decide in M5 and record the choice in `docs/DECISIONS.md`.

**`--backend auto`** (the default) picks Vulkan RT on every GPU that exposes ray-tracing extensions, CUDA on NVIDIA GPUs without them, and CPU only when no GPU qualifies or when `--devices` lists `cpu` explicitly.

**Target hardware.** NVIDIA GPUs are the primary target: every GPU milestone is tuned and validated on them. AMD RDNA2+ and Intel Arc work through Vulkan RT but are not tested. Integrated GPUs without ray-tracing support, such as the laptop's Intel GPU, are not used for rendering in v1: `--backend auto` skips them and uses the CPU instead, while they still drive the UI through Vulkan.

**Integrators vs backends.** "Algorithm" means two different things here. The backend is *where* rays are traced (above). The integrator is *how* light is computed (next section). Every integrator must run on every backend.

## Integrators, sampling, spp and bounces

The default integrator is a unidirectional path tracer with next-event estimation and multiple importance sampling (`--integrator path`). It is the right default: it is what Cycles uses, it maps cleanly to GPU ray-tracing pipelines, and it converges well for typical Blender scenes.

| Integrator | `--integrator` | Milestone | Notes |
| --- | --- | --- | --- |
| Path tracing + NEE + MIS | `path` | M1 | Default. Power heuristic, Russian roulette after bounce 3 (disable with `--no-rr`). |
| Ambient occlusion | `ao` | M1 | Debug and clay renders; distance via `--ao-distance`. |
| Debug views | `debug:normal`, `debug:albedo`, `debug:depth`, `debug:bvh` | M1 | Also used as denoiser AOVs. |
| Bidirectional path tracing | `bdpt` | Post-v1 | For caustic-heavy interiors. Only if the user asks. |
| ReSTIR DI | flag on `path` | Post-v1 | Many-light scenes; mostly useful for interactive preview. |

**spp.** `--spp N` accepts 1 to 4,294,967,295 (uint32). Samples are rendered in passes; the film stores a running mean in float32 per pixel plus a per-pixel sample count, so precision holds past 10^6 spp. `--spp 0` together with `--time-limit` means "render until the time is up".

**Max bounces.** `--max-bounces N` accepts 0 (direct light only) to 1024, default 12. Separate optional caps: `--max-diffuse`, `--max-glossy`, `--max-transmission`, all defaulting to the global value. Russian roulette never terminates a path before bounce 3 and never lets a path exceed the cap.

**Deterministic sampling (critical for distributed rendering).** Every random number is a pure function of (pixel x, pixel y, sample index, dimension, `--seed`). Use Owen-scrambled Sobol with a PCG hash fallback. Consequence: any device can render any tile or any sample range and draw the same random samples: results are bit-identical on the same backend and differ only by floating-point rounding across backends, so splitting work across GPUs, the CPU or other machines never changes the image.

## Memory management: VRAM first, RAM fallback

A `MemoryBudget` per device decides placement before any upload, using a fixed tier ladder; the render degrades in speed, never in correctness, and never crashes on out-of-memory. Scene memory and film memory are handled separately, because the film at large resolutions dwarfs any scene.

**Budget.** Usable VRAM = free VRAM at startup (`VK_EXT_memory_budget` / `cudaMemGetInfo`) minus a 10% safety margin, or `--vram-limit` if set. Usable RAM = `--ram-limit`, default 75% of physical RAM.

**Scene placement ladder, per device:**

1. **Tier 0 — all in VRAM.** Geometry, BVH/acceleration structures, textures, film tiles. Fastest.
2. **Tier 1 — textures to RAM.** Geometry and BVH stay in VRAM; textures move to pinned host memory read over PCIe (CUDA: `cudaHostAlloc` mapped; Vulkan: host-visible, device-accessible memory type). Typical cost: 1.2–2x slower.
3. **Tier 2 — textures downsized.** If pinned RAM is also short, load textures at reduced mip levels and warn with the effective resolution. Opt out with `--no-texture-downsize`.
4. **Tier 3 — device skipped.** If geometry + BVH alone do not fit, that GPU is excluded and the render continues on the other devices and hosts; a host with no usable device rejects the scene (see Inside a worker).

Platform gotchas the agent must respect: CUDA managed-memory oversubscription works on Linux but **not on Windows** (WDDM), so do not build the fallback on `cudaMallocManaged`; use explicit pinned memory on both OSes. Vulkan acceleration structures placed in host memory are driver-dependent; probe support at startup and treat failure as Tier 3.

**Film memory** never lives in VRAM beyond the in-flight tiles (2 per device). The full-frame film exists only on the controller, held in RAM when it fits under the budget, otherwise it is a tiled file on disk (see the next section). Every placement decision is printed at startup and shown in the UI, e.g. `GPU0 RTX 5060: tier 1, scene 6.1 GB / 7.2 GB`.

## Resolution presets and gigapixel output

Presets are exact 16:9 with even dimensions; from 6K up they are clean multiples of 1280×720 so tiles divide evenly. From 20K up the film (16 bytes per pixel) may not fit in RAM, so it is kept on disk and streamed tile by tile.

| Preset `--res` | Width × height (px) | Megapixels | Film size (16 B/px) | Film lives in |
| --- | --- | --- | --- | --- |
| `120p` | 214 × 120 | 0.03 | 0.4 MB | RAM |
| `240p` | 426 × 240 | 0.10 | 1.6 MB | RAM |
| `360p` | 640 × 360 | 0.23 | 3.7 MB | RAM |
| `480p` | 854 × 480 | 0.41 | 6.6 MB | RAM |
| `720p` | 1280 × 720 | 0.92 | 15 MB | RAM |
| `1080p` | 1920 × 1080 | 2.07 | 33 MB | RAM |
| `2k` (QHD) | 2560 × 1440 | 3.69 | 59 MB | RAM |
| `4k` | 3840 × 2160 | 8.29 | 133 MB | RAM |
| `6k` | 5760 × 3240 | 18.7 | 299 MB | RAM |
| `8k` | 7680 × 4320 | 33.2 | 531 MB | RAM |
| `10k` | 10240 × 5760 | 59.0 | 0.94 GB | RAM |
| `15k` (often called 16K) | 15360 × 8640 | 133 | 2.1 GB | RAM |
| `20k` | 20480 × 11520 | 236 | 3.8 GB | RAM if budget allows, else disk |
| `40k` | 40960 × 23040 | 944 | 15.1 GB | RAM if budget allows, else disk |
| `60k` | 61440 × 34560 | 2,123 | 34.0 GB | Disk (default) |
| `80k` | 81920 × 46080 | 3,775 | 60.4 GB | Disk (default) |
| `160k` | 163840 × 92160 | 15,099 | 241.6 GB | Disk |

Custom sizes: `--res 3000x2000` (any WxH from 16×16 up to 163840×163840). `--scale 0.25` renders any preset at a fraction for test runs.

**Disk film (`.penfilm`).** A tiled file: header, tile index, and one block per 256×256 tile holding float32 RGB mean + uint32 sample count. Tiles are written as they finish, so the file is also the checkpoint for `--resume`. Before starting, check free disk space for film + output and refuse with a clear message if short.

**Format limits the writer must enforce.** EXR and PNG are written by streaming tiles/scanlines from the film, never by building the whole image in memory. JPEG caps at 65,535 px per side (so up to `40k`); WebP caps at 16,383 px (up to `8k`); TIFF switches to BigTIFF above 4 GB. Reject an impossible format/size pair at argument parsing, before rendering.

**Render-time reality (approximate).** At `160k`, one sample per pixel is about 15 billion paths. On one high-end RTX GPU with a simple scene that is roughly minutes per spp, so 1,024 spp is on the order of days. Checkpointing, `--resume` and multi-device rendering are therefore core features, not extras.

## Application modes and CLI

One binary covers every role; the mode is chosen by the first flags. Parse with CLI11. Every render flag also has a key in `.pen.json` and the user config file, so UI, CLI and files share one `RenderSettings` struct.

| Command | Mode |
| --- | --- |
| `penumbra` | UI as controller; Host button opens it to workers; start screen has Join a render |
| `penumbra scene.pen.json` | UI as controller with the scene loaded |
| `penumbra --terminal scene.glb -o out.exr` | Headless controller, local devices only, no listening |
| `penumbra --terminal --host scene.glb -o out.exr` | Headless controller that accepts workers |
| `penumbra --worker --join 192.168.1.20 --code K7QD-M2XR-9FHT-4WNB` | Headless worker; exits when the render completes |
| `penumbra --worker --join ... --code ... --stay` | Always-on worker: waits for the next render, reconnects on its own |
| `penumbra --resume render.session.json` | Resume an interrupted render (add `--terminal` for headless) |
| `penumbra merge a.penfilm b.penfilm -o out.exr` | Combine partial films (manual or cluster-driven distribution) |
| `penumbra --list-devices` / `--version` / `--benchmark` / --diagnose | Print and exit |

| Group | Flag | Values / default | Purpose |
| --- | --- | --- | --- |
| General | `--terminal` | off | Headless: no window, no display server needed |
| General | `<scene>` or `--scene` | `.glb`, `.gltf`, `.pen.json` | Scene to load |
| General | `--config` | user config file | Defaults file to use |
| General | `--log-level`, `--log-file` | `info`, auto path | Logging (see Logging) |
| General | `--progress` | `bar`, `plain`, `json` | Progress output style |
| Render | `--res` | preset or `WxH`, `1080p` | Resolution |
| Render | `--scale` | 0.01–1.0, 1.0 | Fractional test renders |
| Render | `--spp` | 1–4294967295, 128 | Samples per pixel |
| Render | `--max-bounces` | 0–1024, 12 | Path depth cap; also `--max-diffuse`, `--max-glossy`, `--max-transmission` |
| Render | `--integrator` | `path`, `ao`, `debug:*` | Light transport algorithm |
| Render | `--seed` | 0 | Deterministic sampling seed |
| Render | `--camera` | first in scene | Camera by name |
| Render | `--time-limit` | e.g. `2h30m` | Stop and write output at the deadline |
| Devices | `--backend` | `auto`, `vulkan`, `cuda`, `cpu`, `optix` | Ray tracing backend |
| Devices | `--devices` | `all`, `gpu`, `0,1,cpu` | Which local devices render (controller or worker) |
| Devices | `--threads` | all cores | CPU worker threads |
| Devices | `--tile-size` | 256 | Work item tile size in px |
| Devices | `--vram-limit`, `--ram-limit` | e.g. `6G` | Memory budget overrides |
| Devices | `--no-texture-downsize` | off | Forbid memory tier 2 |
| Output | `-o`, `--output` | `render.png` | Output file; extension picks format |
| Output | `--format`, `--bit-depth` | `png`/`exr`/`jpg`/`tiff`/`webp`; 8/16/32 | Output encoding |
| Output | `--denoise` | `none`, `oidn`, `optix` | Denoiser |
| Output | `--tonemap`, `--exposure` | `agx`/`filmic`/`aces`/`none`; EV 0 | Display transform |
| Output | `--aov` | `albedo,normal,depth` | Extra passes |
| Film | `--film-dir` | output folder | Where film and session file live |
| Film | `--checkpoint` | `10m` | Checkpoint period |
| Film | `--resume` | session file | Continue an interrupted render |
| Film | `--region`, `--sample-range` | `x0,y0,x1,y1`; `a-b` | Render only part (for `merge`) |
| Controller | `--host` | off | Accept workers (headless) |
| Controller | `--port`, `--bind` | 7420; all interfaces | Listen port and address |
| Controller | `--code` | random | Fixed join code |
| Controller | `--approve-workers` | off | Ask before accepting each worker |
| Controller | `--max-workers` | 64 | Connection limit |
| Controller | `--no-upnp` | off | Never try UPnP |
| Controller | `--no-local` | off | Coordinate only, render nothing locally |
| Controller | `--upload-limit` | unlimited | Cap scene upload bandwidth |
| Worker | `--worker` | off | Run as a headless worker |
| Worker | `--join` | `address[:port]` | Controller to join (LAN IP, public IP, VPN IP, hostname) |
| Worker | `--code` | — | Join code (or env `PENUMBRA_CODE`, so it stays out of shell history) |
| Worker | `--worker-name` | hostname | Name shown in the controller UI |
| Worker | `--stay` | off | Keep serving renders, auto-reconnect |
| Worker | `--cache-dir`, `--cache-size` | per-OS path; 20G | Scene cache |

**Exit codes:** 0 success, 1 bad arguments, 2 scene load error, 3 no usable device, 4 out of disk, 5 interrupted (checkpoint written), 6 rejected by controller (code, version, kicked), 7 connection lost (worker without `--stay`). Ctrl+C on the controller writes a checkpoint and the current image before exiting.

**Windows console gotcha.** Build `penumbra.exe` with the console subsystem so `--terminal` and `--worker` output works in cmd and PowerShell. At startup, if there are no arguments and `GetConsoleProcessList` reports this process is the only one on the console (launched from Explorer), call `FreeConsole()` before opening the UI. Do not use the GUI subsystem plus `AttachConsole`: the shell prompt returns before the render finishes.

**Linux headless.** In `--terminal` and `--worker` modes never initialize GLFW, X11 or Wayland and never request surface extensions, so workers run over SSH and on servers with no display. Load UI libraries only in UI mode.

## UI

The app opens as the controller, with a left sidebar of five pages: Dashboard, Hosts, Render, Viewport and Settings. The UI drives the same controller code as headless mode and has no rendering path of its own.

**Reference mockups.** The approved screens are on the [Penumbra UI Mockups](https://claude.ai/artifact/LS9QoQHVvAH1wV3UW4S3kL) canvas, and the same six screens are committed as files in `docs/ui-mockups/`. Match their layout, labels, colours (graphite background, amber accent, IBM Plex Sans and Mono) and page structure; the numbers in them are sample data.

**Technology.** Dear ImGui on GLFW with a Vulkan swapchain, falling back to OpenGL 3.3 for display only, for machines with old or missing Vulkan drivers. Usable down to 1366×768. Host statistics refresh once per second; charts keep the last 10 minutes.

**Dashboard (start page).** One screen answering "what is my cluster doing right now":

- **Render card:** scene name, state (idle, preparing, rendering, paused, writing output, done), progress %, samples done of total, elapsed, ETA; a button to open the Viewport.
- **Hosts online:** number of connected hosts (controller included), the join code with a copy button, and the address to give workers.
- **GPUs:** total GPU count across all hosts, split into rendering, idle and skipped (memory tier 3 or disabled).
- **Cluster CPU:** usage across all hosts, weighted by core count, plus total cores.
- **Cluster RAM:** used and total GB summed across all hosts.
- **Throughput:** total samples per second with a 10-minute sparkline.
- **Controller disk:** free space versus what the current render's film and output need.
- **Events:** the last 20 events (host joined or left, device failed, worker kicked, checkpoint written, errors).

**Hosts page.** A sortable table with one row per connected host, the controller's own machine first: status (idle, downloading scene x%, building, rendering, upload-bound, lost), hostname, OS, address, CPU usage %, RAM used/total, GPU count, average GPU usage %, tiles per second and ping. Clicking a row opens the host detail page.

**Host detail page.** Everything about one host:

- **Header:** hostname, OS, Penumbra version, address, connected since, ping, upload rate.
- **CPU and RAM:** model, cores, usage graph; RAM used/total graph.
- **One card per GPU:** name, backend, memory tier, VRAM used/total, usage %, temperature, tiles and samples rendered this render, and an enable/disable toggle.
- **Scene cache:** size used and cached scenes.
- **Log:** the latest warnings and errors from that host.
- **Actions:** rename, disable the whole host, kick.

**Render page.** Everything needed to start a render, top to bottom:

- **Scene selector:** Open, recent scenes and drag-and-drop of `.glb` / `.pen.json`, with scene info (triangles, instances, lights, textures, package size). Missing files are listed here, before any render.
- **Camera and preview:** camera dropdown and a small low-resolution preview to check framing, rendered on the controller's own devices and refreshed on change.
- **Settings:** resolution preset or custom size, spp, max bounces (with per-type caps under Advanced), integrator, seed, backend preference, denoise, tonemap and exposure, output folder, file name, format, bit depth, AOVs.
- **Live estimates:** film size, disk needed versus free on the controller, and a rough render time from the cluster's measured speed.
- **Start render:** disabled with a visible reason when something blocks it (no scene, no usable devices, not enough disk, a render already running).

**Viewport page.** The render in progress:

- **Image:** the live film with fit, 100% and zoom/pan; above 8K a downsampled view that zooms into full-resolution tiles.
- **Overlays:** tile grid coloured by host (a palette of 8 distinct colours; beyond 8 hosts, colours repeat with a hatch pattern), tiles in progress outlined; toggleable.
- **Progress:** bar, %, samples done, elapsed, ETA, and each host's share of finished tiles.
- **Controls:** Pause, Resume, Stop, Save image now.
- **Live output adjustments:** exposure, tonemap and a denoised preview can be changed during the render, because they apply after the film.

**Settings page.** Hosting (LAN hosting on/off, internet hosting with UPnP, port, Regenerate code, approve new workers), defaults (output folder, tonemap), local devices (enable/disable, CPU cores reserved for the UI), scene cache, and a button to open the logs folder. **Join a render** is also here: it stops hosting and switches the app into worker mode, showing the controller it joined, scene download, its GPUs with usage, tier and tiles done, upload rate, and Leave.

**Where the numbers come from.** CPU and RAM come from the OS (`GetSystemTimes` and `GlobalMemoryStatusEx` on Windows, `/proc/stat` and `/proc/meminfo` on Linux). GPU usage and temperature come from NVML on NVIDIA GPUs (loaded at runtime from the driver); on AMD and Intel GPUs the UI shows Penumbra's own busy % (share of time the device spent rendering) and labels it as such. Every host sends these in `Stats` every 2 seconds, also when idle.

## Scene format, materials, lights and camera

glTF 2.0 binary (`.glb`) exported from Blender is the primary scene format, loaded with cgltf. A small `.pen.json` file wraps it with what glTF cannot carry: environment HDRI, render settings and camera overrides.

```json
{
  "scene": "living_room.glb",
  "environment": { "hdri": "studio_4k.exr", "strength": 1.0, "rotation_deg": 90 },
  "camera": "Camera.001",
  "render": { "res": "4k", "spp": 512, "max_bounces": 12, "tonemap": "agx" }
}
```

**glTF extensions to support:** `KHR_materials_transmission`, `KHR_materials_ior`, `KHR_materials_clearcoat`, `KHR_materials_sheen`, `KHR_materials_specular`, `KHR_materials_emissive_strength`, `KHR_materials_anisotropy`, `KHR_texture_transform`, `KHR_lights_punctual`, `EXT_mesh_gpu_instancing`. Unsupported extensions log a warning and fall back to base PBR. OBJ via tinyobjloader is a secondary input for test meshes.

**Material model.** One principled BSDF that maps 1:1 to glTF PBR plus the extensions above, close to Blender's Principled BSDF: base color, metallic, roughness (GGX with visible-normal sampling), specular, rough dielectric transmission with IOR, clearcoat, sheen, anisotropy, emission, normal maps, alpha cutout. Textures are sRGB or linear per glTF rules, with mipmaps and bilinear filtering.

**Lights.** Emissive meshes (sampled by power through a light BVH once a scene has more than 64 emitters), point, spot, directional/sun, and HDRI environment with importance sampling via an alias table.

**Cameras.** Perspective with thin-lens depth of field (aperture and focus distance from glTF/Blender), orthographic, and equirectangular 360° panorama.

**Instancing.** Instances are first-class in all backends (TLAS/BLAS in Vulkan, two-level BVH in tinybvh and Embree), so a forest of 10,000 copies of one tree costs one tree's memory.

## Output, denoising and tonemapping

The film always stores linear scene-referred radiance; denoising, tonemapping and quantization happen only in the output stage, tile by tile, so they work at every resolution up to 160K.

**Pipeline per output tile:** read film tile with a 32 px border → denoise (optional) → exposure → tonemap → encode → stream to writer. The border avoids seams from the denoiser.

**Denoising.** Intel Open Image Denoise 2.x (`oidn`, default when enabled) runs on CPU and on NVIDIA GPUs via its CUDA device, on both OSes. It uses albedo and normal AOVs captured at the first non-specular hit. The OptiX denoiser (`optix`) is an optional NVIDIA-only alternative in the OptiX milestone. Denoising is off by default for `--terminal`, so reference renders stay unbiased.

**Tonemapping.** AgX (default, matching Blender 4.x and later), Filmic, ACES, and `none` for linear EXR. Use the published AgX LUT/matrices; the result for a Blender-exported scene should look close to the Blender viewport.

**Formats and libraries.** EXR via OpenEXR (tiled, half or float, ZIP/DWAA compression, multi-part for AOVs); PNG via libpng (8/16-bit, row-streamed); JPEG via libjpeg-turbo; TIFF via libtiff (BigTIFF when needed); WebP via libwebp. AOVs go into the same EXR as extra layers, or as sibling files `name.albedo.png` for other formats.

**Metadata.** Every output embeds scene path, resolution, spp, bounces, seed, backend, devices, render time and Penumbra version (EXR header attributes, PNG `tEXt` chunks).

## Logging, configuration and diagnostics

Every decision the system makes on its own (backend choice, memory tier, requeue, kick, fallback) is logged at `info` with the host and device it concerns; a distributed render must be debuggable from the controller's log alone.

**Logging.** spdlog, one rotating log file per process (10 MB × 5): `%LOCALAPPDATA%\Penumbra\logs` on Windows, `~/.local/state/penumbra/logs` on Linux, or `--log-file`. Each line carries timestamp, role (controller/worker), host name and device id where relevant. Workers forward warnings and errors to the controller with `Log` messages, so the controller log tells the whole story.

**Progress output.** `--progress json` prints one JSON object per second on stdout: percent, samples/s, ETA, per-worker and per-device stats. This is the integration point for scripts and the user's Go cluster webUI.

**Configuration.** Precedence, highest first: command-line flags → `.pen.json` → user config (`%APPDATA%\Penumbra\config.json` / `~/.config/penumbra/config.json`) → built-in defaults. The config file holds personal defaults such as worker name, cache size, default output folder and preferred tonemap. The join code is never written to the config file.

**Crash reports.** On a crash, print a stack trace (cpptrace) to the log and stderr; on Windows also write a minidump next to the log. A crashed worker's last log lines are kept and uploaded to the controller on its next join.

**Diagnostics command.** `penumbra --diagnose` prints OS, drivers, Vulkan and CUDA versions, each device with RT support and free VRAM, free disk for cache and film, and whether TCP 7420 can be bound. The README tells users to paste its output into bug reports.

## Build, dependencies and platforms

C++20, CMake 3.26+ with presets, vcpkg in manifest mode for third-party libraries; GPU SDKs are installed system-wide. Each backend is a CMake option, so a machine without the CUDA toolkit still builds Penumbra with Vulkan + CPU.

| Toolchain | Windows | Linux (Ubuntu 22.04+ / Debian 12+) |
| --- | --- | --- |
| Compiler | MSVC 2022 (17.10+) | GCC 12+ (`gcc-12` on 22.04) or Clang 15+ |
| CMake | 3.26+ | 3.26+ (22.04 ships 3.22: use Kitware apt repo or `pip install cmake`) |
| CUDA | 12.8+ (required for Blackwell, sm\_120) | 12.8+ |
| Vulkan | Vulkan SDK 1.3.280+ | Vulkan SDK or distro packages; NVIDIA proprietary driver, or Mesa 23.1+ for AMD RT |
| Slang | Prebuilt release, pinned version | Same |

| Library | Used for | License |
| --- | --- | --- |
| volk + Vulkan Memory Allocator | Runtime Vulkan loading, allocation | MIT |
| Slang | Shared shader language | MIT/Apache-2.0 |
| tinybvh | GPU software BVH for CUDA backend | MIT |
| Embree 4 + oneTBB | CPU ray tracing and threading | Apache-2.0 |
| Open Image Denoise 2 | Denoising | Apache-2.0 |
| cgltf, tinyobjloader | Scene import | MIT |
| OpenEXR, libpng, libjpeg-turbo, libtiff, libwebp, stb\_image | Image I/O | BSD / zlib / PD |
| Standalone Asio | Async TCP networking | BSL-1.0 |
| mbedTLS 3.x | TLS 1.3 with external PSK (join code) | Apache-2.0 |
| noise-c (fallback only) | Noise NNpsk0 if mbedTLS PSK falls short | MIT |
| FlatBuffers | Wire protocol schemas | Apache-2.0 |
| zstd | Compression of scene chunks and tiles | BSD |
| BLAKE3 | Scene, chunk and tile hashes | CC0 / Apache-2.0 |
| miniupnpc | UPnP port mapping | BSD |
| Dear ImGui, GLFW | UI | MIT / zlib |
| CLI11, nlohmann/json, spdlog | CLI, config, logging | BSD-3 / MIT |
| cpptrace | Stack traces on crash | MIT |
| Catch2 | Tests | BSL-1.0 |
| OptiX SDK (optional) | NVIDIA RT backend + denoiser | NVIDIA SDK license |
| NVML (loaded at runtime) | NVIDIA GPU usage and temperature for the Hosts pages | Part of the NVIDIA driver |

**CMake options:** `PEN_ENABLE_VULKAN` (ON), `PEN_ENABLE_CUDA` (ON if toolkit found), `PEN_ENABLE_OPTIX` (OFF), `PEN_ENABLE_UI` (ON), `PEN_CUDA_ARCHS` (default `75;80;86;89;90;120`).

**Runtime robustness.** Load Vulkan through volk and CUDA through the driver API so the binary starts on a machine with no GPU driver at all and simply falls back to CPU.

**Distribution is build-from-source.** Windows SmartScreen blocks unsigned `.exe` files downloaded from the internet, so users build Penumbra themselves; a locally built exe carries no download mark and runs without a warning. One-command build scripts do the whole job: `scripts/build.ps1` on Windows and `scripts/build.sh` on Linux (check prerequisites, configure, build, run the CPU smoke test, print where the binary is). Prebuilt GitHub release archives are optional extras, never the main path. CI (GitHub Actions) builds both OSes and runs CPU-backend tests; GPU tests run on the GPU machines (see Testing) via `ctest -L gpu`.

## Repository, GitHub and README

All code lives in **github.com/NTFespolion307/Penumbra**, branch `main`; the agent commits and pushes there so no work stays only on the local machine.

**Saving work.** On the first session: clone the repo (or `git remote add origin https://github.com/NTFespolion307/Penumbra.git`), commit this guide as `docs/GUIDE.md` plus `CLAUDE.md`, and push. After that: commit at every green test run, and `git push origin main` at the end of every session and every passed milestone gate. If a push fails (auth, network), stop and tell the user instead of continuing with unpushed work. Large binaries (renders, test scenes over 50 MB) go through Git LFS or stay out of the repo.

**README.md is a required deliverable, updated in the same commit as any change it describes.** It must contain:

1. What Penumbra is, one screenshot, supported OSes and GPUs, and the controller/worker idea in three sentences.
2. Why you build it yourself on Windows (SmartScreen), in two sentences.
3. Prerequisites per OS with exact versions and copy-paste install commands: Visual Studio 2022 workloads, CMake, Vulkan SDK, CUDA 12.8+, Git, vcpkg on Windows; `apt` commands for Ubuntu 22.04 and Debian 12, including the newer CMake and `gcc-12`.
4. Build steps: the one-command scripts first, then the manual CMake preset commands, then how to build with or without CUDA/UI.
5. Running solo: UI mode, `--terminal` examples (quick preview, 4K final, `--resume`), `--list-devices`, `--diagnose`.
6. Rendering with several machines: hosting from the UI and with `--terminal --host`, joining with `--worker --join --code`, `--stay` for always-on servers, LAN vs internet, port forwarding and UPnP, VPN for carrier-grade NAT, Windows Firewall and `ufw` rules, and a full 160K example across three machines.
7. Full CLI reference: every flag, its values, default and an example, generated from the same source as `--help` so they never drift.
8. Resolution preset table, backends and how `--backend auto` chooses, multi-GPU and memory tiers, output formats and their size limits, disk space needed on the controller.
9. Exporting a scene from Blender (`.glb` settings) and writing a `.pen.json`.
10. Troubleshooting: no Vulkan ray tracing, CUDA not found, worker rejected (version or code), worker can't connect, upload-bound workers, out of disk.
11. Updating: `git pull` then rerun the build script on every machine, because controller and workers must run the same version.

## Repository layout and core interfaces

The core library knows nothing about GPU APIs or sockets; the controller and worker libraries depend on core; backends and network code plug in through two interfaces. The app is a thin shell that picks a mode.

```text
Penumbra/
├─ CLAUDE.md, README.md, LICENSE, CMakeLists.txt, CMakePresets.json, vcpkg.json
├─ docs/            GUIDE.md (this guide), DECISIONS.md, BENCHMARKS.md, PROTOCOL.md
├─ apps/penumbra/   main.cpp (mode dispatch), cli/, ui/ (ImGui screens)
├─ core/            math, scene (gltf, graph, instancing), settings, config,
│                   sampling, film (+ .penfilm, merge), output (writers, tonemap,
│                   denoise), package (.penpkg), memory (MemoryBudget), log
├─ controller/      session, scheduler (items, leases), film writer, local worker,
│                   hosting (listener, join codes, UPnP), session file
├─ worker/          client, scene cache, device manager, uploader
├─ net/             schema/*.fbs, framing, TLS-PSK channel, chunked transfer
├─ shaders/         Slang: integrator.slang, bsdf/, lights/, camera.slang
├─ backends/        backend.h, cpu/ (Embree), cuda/ (tinybvh), vulkan/, optix/
├─ tests/           unit/, golden/ (reference images), net/ (loopback, faults), scenes/
├─ tools/           penumbra-diff, netem-proxy (latency/loss/drop), scene generators
└─ scripts/         build.ps1, build.sh, test-gpu.ps1, test-gpu.sh
```

**`IBackend`** is what one device implements; **`WorkerLink`** is how the controller sees any worker, local or remote:

```cpp
struct IBackend {                       // one per device
  virtual DeviceInfo info() const = 0;
  virtual MemoryPlan plan(const SceneStats&, const MemoryBudget&) = 0;
  virtual void upload(const Scene&, const MemoryPlan&) = 0;
  virtual void render(const WorkItem&, TileBuffer& out) = 0;   // tile x sample range
  virtual void renderPreview(const Camera&, PreviewBuffer&) = 0; // UI only
  virtual ~IBackend() = default;
};

struct WorkerEvents {                   // implemented by the controller
  virtual void onReady(WorkerId, const std::vector<DeviceReady>&) = 0;
  virtual void onRequestWork(WorkerId, DeviceId, int count) = 0;
  virtual void onTileResult(WorkerId, TileResult&&) = 0;
  virtual void onRelease(WorkerId, std::span<const LeaseId>) = 0;
  virtual void onStats(WorkerId, const WorkerStats&) = 0;
  virtual void onGone(WorkerId, GoneReason) = 0;
};

struct WorkerLink {                     // in-process (local) or network (remote)
  virtual WorkerInfo info() const = 0;
  virtual void offerScene(const ScenePackage&, const RenderSettings&) = 0;
  virtual void sendLease(const Lease&) = 0;
  virtual void cancel(std::span<const LeaseId>) = 0;
  virtual void ackTile(LeaseId, AckStatus) = 0;
  virtual void pause(bool paused) = 0;
  virtual void complete() = 0;
  virtual ~WorkerLink() = default;
};
```

`docs/PROTOCOL.md` is generated from `net/schema/*.fbs` plus this guide's protocol section and kept in sync with every protocol change.

## Milestones and acceptance gates

The distributed core (M3, M4) is built right after the CPU renderer and before any GPU backend, so every later feature is born distributed and can be tested on the GPU-less laptop with several CPU worker processes.

&#91;embedded content: Milestone roadmap · M0 to M13, each with its gate\]

A milestone is done only when its gate passes on both Windows and Linux and a fresh clone builds from the README. GPU milestones (M5–M9) pass only on results from the RTX 5060 PC or the GPU server. M5 and M6 do not depend on each other and may be swapped.

**M14 Atmosphere and physical sky (added by the user, D-105; done after M8 and M9, before M11).** A participating medium for the whole scene: exponential height fog (density falls off with altitude, so transmittance has a closed form and needs no voxel grid) with a Henyey-Greenstein phase function, free-flight distance sampling and next-event estimation to the sun and the environment, in the shared shading code so CPU, Vulkan and CUDA render it alike. Plus a physical sky (Rayleigh and Mie scattering, ozone; like Blender's Nishita sky) computed from the sun direction, so sky and haze always match. Set in `.pen.json` (`"atmosphere"`, `"sky"`) with the usual flag, config, UI and README rules. Gate: transmittance through a uniform medium matches the closed form; a non-absorbing medium in a uniform white environment renders uniform (furnace test); CPU, Vulkan and CUDA match (relMSE < 1e-3 at 4,096 spp) and any distribution of work matches a solo render; with no atmosphere every existing golden image is unchanged; the physical sky is close to Blender's for the same sun angle; a real-terrain mountain scene (elevation data plus aerial photos, fetched by `scripts/get-scenes`) renders at 8K with haze on the GPU server.

**M4 in detail, because it carries the most new risk:** first the protocol over loopback (controller + 3 worker processes on the laptop), then TLS-PSK and join codes, then scene packages with cache and resume, then heartbeats, leases and every row of the fault table, then `--stay` and controller `--resume`, then UPnP and an internet test from the GPU server. Each step is committed and pushed separately.

## Testing and validation

Two invariants are tested constantly: every backend matches the CPU reference, and every distribution of work (1 device, N devices, N hosts) matches a solo render. Most bugs in a distributed multi-backend renderer show up as "tiles from host B are slightly darker", and these tests catch exactly that.

**Golden scenes** (`tests/scenes/`): Cornell box, glass and metal spheres on an HDRI, furnace test (white sphere in a uniform white environment, must render exactly uniform), many-lights grid (1,000 emissive quads), instanced forest (100k instances), Sponza with textures, and a 50 M-triangle stress scene generated by `tools/`.

**Comparisons** with `penumbra-diff` (mean relative error and FLIP). Same backend and seed, any distribution of work → identical within float tolerance (relMSE < 1e-6). CPU vs Vulkan vs CUDA at 4,096 spp → relMSE < 1e-3. Furnace test → every pixel within 0.5% of expected.

**Unit tests:** BSDF energy conservation and reciprocity, sampler stratification, MIS weights, resolution presets, CLI parsing, config precedence, `.penfilm` write/merge/resume, `.penpkg` build and verify, join code generation and parsing, frame encode/decode for every message, tile encoding (float16/float32 switch, NaN handling).

**Network tests** (`tests/net/`, run in CI on CPU): controller plus 3 worker processes over loopback; join mid-render; wrong code; version mismatch; scene cache hit, miss and resumed download; `scene_rejected`; worker killed mid-lease; worker that heartbeats but never returns a tile; corrupted tile; controller killed and resumed with `--stay` workers reconnecting; 200 ms latency, 2% packet loss and a 10 Mbit/s uplink through `tools/netem-proxy`. Each must finish with an image identical to the solo render.

**Scale test.** A synthetic test backend (`--backend null`, test builds only) returns correct-shaped tiles instantly, so 64 simulated workers with 8 devices each can run on one machine. The scheduler, film writer and UI must stay responsive under that load, which covers five GPU servers with room to spare.

**Large-resolution tests:** `20k` on the laptop (RAM film); `60k` and `160k` of the Cornell box at 1 spp on the GPU server (disk film), then check the output opens and a random tile matches a `--region` render of the same area.

**Cross-machine tests** before closing M4 and M7: Windows controller with Linux workers and the reverse; a LAN worker and an internet worker in the same render.

**Test machines.** The dev laptop (i7 11th gen, 32 GB, Intel integrated GPU without ray tracing) runs all CPU, unit and network tests and compiles GPU code but never validates it. The gaming PC (RTX 5060, 8 GB, Blackwell, hardware RT) was meant for single-GPU and low-VRAM tests but is not available (D-107); rented GPU boxes take its place, with `--vram-limit` standing in for a small card. Up to five GPU servers (mostly NVIDIA, mixed models) run multi-GPU, large-resolution and internet-worker tests.

**Remote GPU runs.** Register the gaming PC and the server as self-hosted GitHub Actions runners labelled `gpu-windows` and `gpu-linux`; every push to `main` runs `ctest -L gpu` there and the agent reads the Actions log. With a public repo, GPU jobs run only on pushes to `main`, never on pull requests from forks, because a fork's PR would run code on those machines. Fallback: the user runs `scripts/test-gpu.ps1` / `scripts/test-gpu.sh` and pastes the summary back.

**Benchmarks** go into `docs/BENCHMARKS.md` at every milestone from M5: rays/s and samples/s per backend and GPU per golden scene, plus network throughput (tiles/s, upload MB/s) per host.

## Suggested extra features

None of these is built until the user approves it; they are listed so the architecture leaves room for them.

| Feature | Why it matters | Effort | Fits after |
| --- | --- | --- | --- |
| Adaptive sampling `--noise-threshold` | Converged pixels stop early; typically 2–4x fewer samples at high spp. Work items then carry per-pixel sample targets. | M | M9 |
| Deep-zoom output `--format dzi` | A tile pyramid viewable in any browser (OpenSeadragon); the practical way to look at a 15-gigapixel image. | S | M2 |
| Go cluster integration | The user's cluster software launches `penumbra --worker --stay` on its nodes, including rented GPUs, and reads `--progress json` for its webUI. | S | M4 |
| Worker as a system service | Install `--worker --stay` as a Windows service or systemd unit so GPU machines are always available. | S | M4 |
| Blender add-on | "Render with Penumbra" button: exports `.glb` + `.pen.json`, starts a controller, shows the result in Blender. | M | M10 |
| Batch queue `--batch jobs.json` | Several cameras, resolutions or scenes in one session; workers stay connected between jobs. | S | M12 |
| Light groups and AOV passes | Separate passes per light for compositing. | M | Post-v1 |
| Atmosphere and physical sky | Haze and aerial perspective (distant mountains turn blue), light shafts, a sky that matches the sun. Approved as M14 (D-105). | M | M9 |
| Volumes (OpenVDB clouds and smoke) | Real 3D clouds and effects; voxel grids, NanoVDB, uneven-density sampling and the memory tiers. | L | M14 |
| Scene prefetch | Start sending the scene to idle workers as soon as it is selected on the Render page, so they are already ready when Start render is clicked. | S | M4 |
| Peer-assisted scene distribution | Workers on the same LAN fetch scene chunks from each other, so five servers in one place download the scene once from the controller instead of five times. | M | Post-v1 |
| Vulkan compute backend (no RT cores) | Software BVH in Vulkan compute, so integrated GPUs like the laptop's Intel GPU and older AMD cards can render too; likely a few times faster than the laptop CPU. | M | Post-v1 |

## Open decisions

These wait on the user. Build with the default and keep the alternative possible; record each answer in `docs/DECISIONS.md`.

| Decision | Default until answered | Alternatives |
| --- | --- | --- |
| Which GPU vendors must work | Decided: NVIDIA first (tuned and tested); AMD and Intel Arc work through Vulkan RT but are untested; integrated GPUs without RT (the laptop's Intel GPU) do not render in v1 | Revisit OptiX (M13) once benchmarks exist |
| OS of the GPU machines | Decided: any host may run Windows or Linux; no machine is assumed to run either | None |
| Hosting in the UI | LAN hosting on when the UI opens; internet hosting opt-in | Off until the user enables it in Settings |
| Internet workers | UPnP tried only after the user enables internet hosting | UPnP always tried; never tried |
| Who hosts 160K renders | One of the GPU servers, or the laptop with an external drive via `--film-dir` | Laptop internal SSD if it has 250+ GB free |
| Scene input | glTF `.glb` from Blender + `.pen.json` | Direct `.blend` (fragile across Blender versions); USD (heavy) |
| Language | C++20 | Rust (weaker CUDA/OptiX tooling) |
| Denoiser default | On for UI previews, off in `--terminal` | Always off; always on |
| License | MIT (repo: github.com/NTFespolion307/Penumbra) | Apache-2.0; private repo |
| Extra features | Approved: atmosphere and physical sky (M14, D-105); the rest none until approved | Any row of the table above |
| M14 mountain scene | Asked when M14 starts: a US range (USGS 3DEP elevation and NAIP photos, public domain, can be redistributed) | Swiss Alps (swissALTI3D and SWISSIMAGE 10 cm, sharper; swisstopo open data with attribution) |
| M14 terrain converter | Asked when M14 starts: a C++ tool in `tools/` | A Python script (numpy, tifffile), run once, outside the build |

## Working rules for the implementing agent

Build in milestone order, keep the CPU backend and the solo render as ground truth, and never merge a change that breaks a golden or network test.

1. Start each session by reading `CLAUDE.md`, this guide, `docs/DECISIONS.md` and the last entry of `docs/BENCHMARKS.md`.
2. One milestone at a time. Before coding a milestone, post a short plan and wait for the user's OK. When its gate passes, stop and report what passed, what failed, and the decisions made.
3. Correctness first, then speed. A faster kernel or protocol change that alters the golden images is a bug.
4. Everything goes through the controller/worker path, including solo renders. Never add a shortcut that renders without the scheduler, `WorkerLink` and the film.
5. Keep layering strict: `core/` has no GPU or socket headers; only `backends/` includes Vulkan, CUDA, OptiX or Embree; only `net/`, `controller/hosting` and `worker/client` touch sockets.
6. Every new flag goes into `RenderSettings`, `--help`, the `.pen.json` schema, the config file, the UI and `README.md` in the same commit. Every protocol change updates `net/schema`, bumps the protocol version and updates `docs/PROTOCOL.md`.
7. Log every automatic decision (backend, memory tier, requeue, kick, fallback) at `info`. No silent fallbacks.
8. When reality differs from this guide (Slang CPU target, mbedTLS PSK support, Vulkan AS in host memory, OIDN tiling), test it, pick what works, and write it to `docs/DECISIONS.md`.
9. Ask the user before adding a dependency not listed here, changing a CLI flag name, or changing the default port.
10. Test on both OSes before closing a milestone: Windows through the MSVC preset, Linux through the GCC preset in a clean Ubuntu 22.04 container.
11. Push to `github.com/NTFespolion307/Penumbra` (`main`) at the end of every session and every passed gate; never end a session with unpushed commits. If a push fails, stop and tell the user.
12. A milestone is not done until a fresh clone builds on Windows and Linux by following `README.md` alone.

**The dev laptop has no rendering GPU.** Never mark a GPU milestone (M5–M9) as passed from the laptop alone: it passes only with results from the RTX 5060 PC or the GPU server, and the report names the machine and GPU that produced them. Network milestones (M3, M4) are tested on the laptop with several worker processes, then across real machines.
