# Decisions

Every decision made during the build, newest last within each milestone. Read at the start of every session.
Format: what was decided, why, and what it replaces in `docs/GUIDE.md` if anything.

## Open decisions from the guide (defaults in force until the user answers)

| Decision | In force | Status |
| --- | --- | --- |
| GPU vendors | NVIDIA first; AMD/Intel via Vulkan RT untested; integrated GPUs without RT do not render | Decided (guide) |
| OS of GPU machines | Any host may be Windows or Linux | Decided (guide) |
| Hosting in the UI | LAN hosting on when the UI opens; internet hosting opt-in | Default |
| Internet workers | UPnP tried only after internet hosting is enabled | Default |
| Who hosts 160K renders | A GPU server, or the laptop with `--film-dir` on an external drive | Default |
| Scene input | glTF `.glb` + `.pen.json` | Default |
| Language | C++20 | Default |
| Denoiser default | On for UI previews, off in `--terminal` | Default |
| License | MIT, public repo github.com/NTFespolion307/Penumbra | Decided (user created the public repo; LICENSE committed in M0) |
| Extra features | None until approved | Default |

## M0: Foundation

User approved every M0 decision below (D-001 to D-018), including {fmt} (D-018), the real JPEG/WebP size
limits (D-010) and the 6k note (D-012), on 2026-10-16. M0 gate passed on Windows (MSVC 19.51) and WSL
Ubuntu 22.04.5 (GCC 12.3), plus GitHub Actions on both OSes.

**D-001 Toolchain: Visual Studio 2026 (18) Build Tools, MSVC 19.51.** The dev laptop has VS 18 Build Tools, not
VS 2022. The build uses only standard C++20 and CMake/Ninja, so both work; the README lists VS 2022 17.10+ or
VS 2026 as supported. Build scripts find either with `vswhere -latest -products *`.

**D-002 Windows presets use Ninja + `cl` from an x64 developer environment.** The Visual Studio generator name is
tied to the VS version (2022 vs 2026), Ninja is not. `scripts/build.ps1` imports `vcvars64.bat` itself, so users
can build from any PowerShell window. CMake refuses 32-bit compilers.

**D-003 vcpkg location and `VCPKG_ROOT`.** vcpkg is a separate clone (`C:\dev\vcpkg` on the dev laptop,
`~/vcpkg` on Linux) found through the `VCPKG_ROOT` user variable. `vcvars64.bat` overwrites `VCPKG_ROOT` with
the copy bundled in Visual Studio; `build.ps1` saves and restores the user's value. Dependencies are pinned by
`builtin-baseline` 3cd21e48a75f67acfef289da52893ce79144aaa1 (CLI11 2.7.2, spdlog 1.17.0 with fmt 12.2.0,
nlohmann-json 3.12.0, Catch2 3.16.0). Catch2 is behind the `tests` manifest feature.

**D-004 Windows triplet `x64-windows-static-md`.** Static third-party libraries (no DLLs to copy next to the
exe) with the dynamic MSVC runtime, which every machine that builds Penumbra already has and which CUDA and
Vulkan libraries expect.

**D-005 Linux gate runs in a WSL2 Ubuntu 22.04 distro named `Penumbra`**, not a Docker container (user
preference: no Docker). It started clean (no compiler, CMake or git), so following the README's Linux
prerequisites in it from zero is the fresh-machine test. GitHub Actions `ubuntu-22.04` is the second check.

**D-006 Linux CMake: official 3.31.6 tarball into `/opt/cmake`.** Ubuntu 22.04 ships 3.22 and Debian 12 ships
3.25 (both below 3.26). The Kitware apt repo covers Ubuntu only, and `pip install cmake` is blocked on Debian 12
(PEP 668), so the README uses the official binary, which works on both.

**D-007 One settings table drives everything.** `core/settings/schema.cpp` lists every persistent setting once:
JSON key, CLI names, group, values, default, example, help and the parser. The CLI options, `--help`, the
README CLI reference (`penumbra --help-markdown`, checked by the `docs.cli_reference` test and regenerated with
the `update-cli-docs` target) and the keys accepted in `.pen.json` `render` and the user config all come from
it. CLI key = flag without dashes, `-` → `_` (`--max-bounces` ↔ `max_bounces`), enforced by a unit test.
Mode flags (`--terminal`, `--host`, `--worker`, `--join`, `--code`, `--resume`, `--config`, info flags) are
CLI-only and never read from files.

**D-008 Settings precedence is implemented as layers of JSON values.** Command-line values arrive as strings and
file values as native JSON types; each goes through the same per-key parser, so every source gets identical
validation and error messages that name the flag (CLI) or file and key (files). Unknown keys in files are
logged as warnings, not errors; a join code in a file is ignored with a warning.

**D-009 Value conventions.** `--region x0,y0,x1,y1` is half-open (x1, y1 exclusive, like width/height).
`--sample-range a-b` is inclusive ("0-63" is 64 samples, as the guide writes work items). Durations accept
`d/h/m/s` combinations (`2h30m`); sizes use binary units (`6G` = 6 GiB). `--time-limit none`, `--vram-limit
auto`, `--upload-limit unlimited` spell the defaults. `--checkpoint` is limited to 10 s to 7 days.

**D-010 Format size limits use the libraries' real limits.** JPEG: 65,500 px per side (libjpeg-turbo
`JPEG_MAX_DIMENSION`), so `60k` (61,440 px) is allowed; the guide's "(so up to 40k)" was conservative. WebP:
16,383 px per side, so `10k` and `15k` (15,360 px) are allowed; the guide said "up to 8k". The check runs on the
scaled output size, so `--res 20k --scale 0.5 -o x.webp` is accepted.

**D-011 Default bit depths:** PNG, JPEG, WebP 8; TIFF 16; EXR 16 (half float, Blender's default and half the
disk of float at 160K). `--bit-depth 32` gives float EXR.

**D-012 Resolution presets: the guide's table is authoritative.** The guide also says presets "from 6K up are
clean multiples of 1280×720"; 6k (5760×3240) is 4.5×, so the property holds from 8k up. Dimensions were kept
exactly as in the guide's table; the unit test checks the property from 8k.

**D-013 Modes not built yet exit with code 1** after validating their arguments and logging which milestone
builds them (`--terminal` → M1, `--resume`/`merge` → M2, `--worker` → M4, `--benchmark` → M9, UI → M10). The
guide's exit codes have no "not implemented" value and none was added.

**D-014 Windows console and UTF-8.** Console subsystem as the guide says; when the process is alone on its
console (`GetConsoleProcessList` == 1, i.e. started from Explorer) and the mode is the UI, it calls
`FreeConsole()`. Argument errors in that situation are shown in a message box because a console would vanish
before it could be read. Until M10 the UI mode shows a message box saying the UI arrives in M10. The exe embeds
a manifest with `activeCodePage` UTF-8 and `longPathAware`, so argv and narrow file APIs are UTF-8 like on
Linux; the console output code page is switched to UTF-8 while Penumbra runs and restored on exit.

**D-015 Logging.** stderr (so stdout stays free for `--progress json`) plus a rotating file
`penumbra-<role>.log` (10 MB × 5) in the per-OS log folder. Several processes of the same role on one machine
(network tests) should pass `--log-file` to avoid sharing a file. If the log file cannot be opened, Penumbra
warns and logs to the console only.

**D-016 Version string** carries the app version, 12-character git hash (`-dirty` if tracked files are
modified, `unknown` outside git) and protocol version. It is regenerated at build time, not configure time, so
it never goes stale after a commit.

**D-017 Layering is enforced by a test** (`lint.layering`, `cmake/CheckLayering.cmake`): GPU API headers only in
`backends/` (and `apps/penumbra/ui/` for the swapchain from M10), socket/TLS headers only in `net/`,
`controller/hosting/`, `worker/client/`. Libraries not built yet (`net`, `backends`, `controller`, `worker`) exist
as empty CMake interface targets.

**D-018 {fmt} instead of `std::format`.** GCC 12 (the guide's minimum, the default `g++-12` on Ubuntu 22.04)
ships no `<format>`; it arrived in GCC 13. {fmt} 12.2.0 was already in the build as spdlog's formatter, so it
is now declared in `vcpkg.json` and used directly (`fmt::format`, same syntax). Not a new library in the
build, but it is not in the guide's dependency table: flagged to the user.

## M1: CPU reference renderer

User approved the M1 plan and these choices on 2026-10-16 ("you are the chef").

**D-019 Tonemapping before M11.** Until AgX, Filmic and ACES land in M11, PNG output uses the sRGB transfer
curve with clipping after exposure, and the log says so once per render at `info`. EXR output is linear and is
what golden images use.

**D-020 Emitter sampling.** Lights (emissive triangles, punctual lights, environment) are chosen through one
alias table weighted by estimated power; the light BVH for scenes with more than 64 emitters moves to M9. It
only changes noise, never the expected image.

**D-021 Cornell box acceptance.** Rendered by Penumbra and judged by the user from a PNG; once approved, its
linear EXR becomes the first golden image under `tests/golden/`.

**D-022 penumbra-diff metrics.** relMSE, max absolute difference and an exact (bitwise) mode in M1. FLIP needs
NVIDIA's header, which is not in the guide's dependency list; ask before adding it.

**D-023 Light units.** KHR_lights_punctual intensities are photometric (candela, lux). Penumbra divides by
683 lm/W to get radiometric W/sr and W/m^2, which matches how Blender's exporter converts watts ("Standard"
lighting mode). Emissive materials use emissiveFactor x emissiveStrength directly as radiance. To be checked
against Blender renders in M11 (AgX comparison); any change there is a decision entry, not a silent fix.

**D-024 Alpha.** `MASK` uses alphaCutoff in an Embree filter function (cutout, shadows included). `BLEND` is
rendered as a 0.5 cutout (no stochastic transparency in v1).

**D-025 Transmission is solid glass.** KHR_materials_volume is not in the guide's extension list; without it
glTF says transmissive materials are thin-walled, but Blender glass is almost always meant as solid. Penumbra
treats every transmissive material as a closed dielectric (refracts in and out) and warns when a file uses
KHR_materials_volume. Front face = outside (glTF winding); hits from the back side are inside.

**D-026 Texture filtering.** Bilinear (or nearest, from the glTF sampler) on mip level 0. Mip selection needs
ray differentials or cones, which the GPU backends need too; deferred to M5 so all backends share it. Texture
downsizing for memory tier 2 (M8) builds the mip chain.

**D-027 `debug:bvh`** needs traversal counts that Embree does not expose; it is available from the GPU
backends (M5/M6) and the CPU backend refuses it with a clear message.

**D-028 Not yet built, refused before rendering:** JPEG/TIFF/WebP output (M2, streamed writers), `--spp 0`
(M3, progressive passes), `--host` (M4); `--denoise` and `--aov` warn and are ignored until M11.

**D-029 Environment mapping** follows Blender's equirectangular environment texture with glTF axes (+Y up,
Blender -Y forward = glTF +Z), so an HDRI looks the same as in Blender. `rotation_deg` turns it about +Y.

**D-030 `.pen.json` additions:** `environment.color` (uniform radiance when there is no HDRI; the furnace scenes
use it) and `camera` as an object `{name, projection, fstop, focus_distance, sensor_width_mm}` for what glTF
cannot carry (depth of field; scene units are metres for the f-stop conversion).

**D-031 Sampling layout.** Owen-scrambled Sobol per dimension pair (Burley 2020) seeded by hash(pixel, seed,
dimension); dimensions 0-1 are the camera (pixel jitter, lens), then 5 per bounce (light select, light point,
lobe, direction, Russian roulette), so the same pixel/sample index draws the same numbers on any device.
GCC builds use `-ffp-contract=off` so no compiler-chosen FMA changes results between hosts.

**D-032 BSDF energy model.** Single-scattering GGX (no multiple-scattering compensation) with albedo scaling of
the lower layers; rough materials lose a little energy at high roughness, never gain any (unit-tested). The
furnace sphere uses specularFactor 0 (pure Lambert) and the glass sphere a smooth dielectric, both exactly
energy-conserving.

**D-033 First golden image.** The user approved `tests/golden/cornell_512_4096spp_approved.png` (512x512, 4,096 spp,
Penumbra CPU backend) on 2026-10-16. The test reference is the same scene at 256x256, 4,096 spp, half-float EXR
(`tests/golden/cornell_256_4096spp.exr`). `render.golden_cornell` renders 256 spp and requires relMSE <= 1e-3
(measured 3.4e-4 from noise; +0.3 EV gives 1.3e-2). Any change that moves it is a bug unless the user approves a
new golden image.

**D-034 Build parallelism follows memory.** The first Linux gate run in WSL (16 cores, 15.5 GB) ran out of
memory compiling Embree with 16 GCC jobs (~2 GB each); the kernel killed a compiler and vcpkg retried with one
job. `scripts/build.sh` and `scripts/build.ps1` now set `VCPKG_MAX_CONCURRENCY` and `CMAKE_BUILD_PARALLEL_LEVEL`
to min(cores, available RAM / 2 GB) unless already set, and print the choice.

**D-035 vcpkg compiles the libraries with the distro's default compiler** (GCC 11 on Ubuntu 22.04), while
Penumbra itself builds with `g++-12` from the preset. Both use the same libstdc++ ABI, so no custom vcpkg
triplet is needed; Penumbra's own code still gets GCC 12 as the guide requires.

**D-036 Build folders are tied to one vcpkg.** The dev laptop's main build folder had been configured once with
the vcpkg bundled in Visual Studio (before `build.ps1` restored `VCPKG_ROOT`), so its libraries never landed in
`C:\dev\vcpkg`'s cache and the Windows fresh clone rebuilt everything. CMakeLists.txt now refuses to configure
when the cached vcpkg root differs from the toolchain's, telling the user to delete the folder or use `-Clean`.

## M2: Tiled film and gigapixel output

User approved the M2 plan on 2026-10-16 and kept the milestone order M2 -> M3 -> M4.

**D-037 One film format for every size.** Every render has a `.penfilm` (header, tile index, tile blocks of
float32 RGB mean + uint32 count) that doubles as the checkpoint. Films up to 40k that fit in half the RAM budget
are also mirrored in RAM (the guide's "RAM" rows); larger ones live only on disk. Tiles are appended copy-on-
write, so the index saved at the last checkpoint always points at intact data; reopening for resume truncates
anything appended after it. Each block carries a BLAKE3-64 checksum checked on read. Film and session are
deleted when a render completes, kept otherwise (and always for `--region` / `--sample-range`, for `merge`).

**D-038 Film tile = work item tile.** `filmTileSize` is the whole frame below 1080p (one item, as in M1) and
`--tile-size` above, and the scheduler uses the same grid, so resume can skip finished tiles by index.

**D-039 Resume is strict.** It refuses when Penumbra's git hash differs from the one that started the film, when
the scene fingerprint (BLAKE3 of scene file, .pen.json and HDRI; M4 replaces it with the package hash) changed,
or when a pixel-affecting setting is given on the command line. Output, logging, device and thread settings may
change. Settings are stored in the session as the same JSON values the CLI accepts (`settingsToJson`).

**D-040 `--region` writes only the region**, so a region render is a cheap check of one area of a gigapixel
image (and format size limits apply to the region size). Its film still uses full-image coordinates for merge.

**D-041 Ctrl+C cancels tiles in progress.** The backend stops at the next row and the partial tile is dropped
(never merged), so Ctrl+C takes seconds even with big tiles; the film is checkpointed, the image so far is
written, exit code 5. A second Ctrl+C exits at once. Linux is tested with a real SIGINT; on Windows the same
controller path runs once the console handler sets the flag.

**D-042 Output writers stream 256-row bands.** EXR is tiled 256x256 (ZIP), TIFF uses deflate strips and
BigTIFF above 4 GB, JPEG is quality 95, WebP is lossy quality 95 and is encoded whole (it is capped at 16,383
px per side). 32-bit TIFF is linear float like EXR; 8/16-bit formats are display-encoded.

**D-043 Large gate tests are opt-in.** `render.large_20k` (label `large`) renders 20K at 1 spp and checks one
tile against a `--region` render; the build scripts run `ctest -LE large`, the gate runs `ctest -L large`.

**D-044 M2 gate results (2026-10-19).** Windows (laptop, MSVC 19.51): 103/103 tests; 20K Cornell at 1 spp in 102 s
(film in RAM, 1.23 GB float EXR), one tile identical to a `--region` render. Linux (WSL Ubuntu 22.04, GCC 12):
104/104 incl. real SIGINT. Server (vast.ai, Ubuntu 24.04, Xeon E5-2696 v4, 88 threads): 104/104; 160K Cornell
at 1 spp: film 241.6 GB on disk only, render 20.7 min (15.1 G samples, 12.2 M samples/s, 86 threads), EXR half
ZIP 35.4 GB written in ~12 min, a 256x256 tile identical (relMSE 0) to a `--region` render; film and session
removed on completion. Display sizes now use decimal GB like the guide (`formatBytes`).

## M3: Scheduler and local worker

User approved the M3 plan and these choices on 2026-10-19 ("yes chef").

**D-045 Bit-identical results for any distribution of work.** Each tile's sample range is cut into fixed chunks
computed only from the settings (chunk = largest power of two with tile pixels x chunk <= 2^22, capped by the
range), never from device speed, and the controller merges a tile's chunks strictly in chunk order (results that
arrive early wait). One slot, many slots, failed or hung devices and resume all produce the same bits. This
replaces the guide's per-device 2-10 s item sizing; fast devices take more chunks instead. M9 may retune the
chunk size for GPUs (a retune changes images once and needs a new golden image).

**D-046 Several CPU slots: repeat `cpu` in `--devices`.** `--devices cpu,cpu,cpu,cpu` gives 4 CPU slots that
split the CPU threads (each slot has its own Embree scene). `all` still means one CPU slot until GPUs exist.

**D-047 Test-only hooks, hidden from --help and the README.** `--backend null` returns correctly shaped tiles
instantly (values depend on pixel and sample range, so merge order matters) for scheduler tests and the M4 scale
test. `PENUMBRA_TEST_FAULTS=device=N,after=K,mode=throw|hang` makes local device N fail (throw) or hang until
cancelled after K items; it does nothing unless set, and Penumbra logs a warning when it is active.

**D-048 Film format version 2.** Each tile owns up to two block slots and records how many samples it holds (for
resume in the middle of a tile). New data goes only to the slot the saved index does not reference; RAM films
write dirty tiles at checkpoints, disk films write a tile when it is finished or at a checkpoint. The file stays
below twice the film size (the free-disk check asks for 2x for RAM films). Version 1 films are refused.

**D-049 Requests set, not add.** A device's "I can take N more" replaces its previous request; the controller
hands requeued or re-issued work to waiting devices as soon as it appears, so idle devices pick up a failed
device's chunks without asking again. Leases are re-issued after max(10 x median item time, 2 s).

**D-050 `--spp 0` needs a RAM film**, because it refines every tile pass by pass; a disk-only film would have to
keep every tile open. Penumbra refuses it with a clear message.

**D-051 The golden image test still passes after chunking.** The Cornell golden test renders 256 spp as four
64-sample chunks merged in order instead of one item, which changes the last bits but stays within its relMSE
1e-3 threshold (no new golden image needed).

**D-052 M3 gate results (2026-10-19).** Windows (laptop, MSVC 19.51): 116/116 tests, 20K large test passes (87 s).
Linux (WSL Ubuntu 22.04, GCC 12): 117/117 incl. real SIGINT. Gate: 1, 2 and 4 CPU slots (960x540, 32 spp, 4
chunks) and 1, 4 and 8 null slots (1080p, 160 chunks) give bit-identical images; a device failing on its first
item has its leases requeued and a hung device's lease is re-issued, both bit-identical to a single-slot render.

## M4: Network controller and workers

User approved the M4 plan on 2026-10-19. Machines: WSL is the LAN worker for now (the user's PCs are not near
each other); the internet worker is the vast.ai server joining the laptop controller over Tailscale.

**D-053 Tiles travel as float32, not float16.** The guide sends tile means as float16 when they fit. Rounding to
float16 would make a tile from another host differ from a local one, breaking D-045 (any distribution gives the
same bits). Tiles are sent as float32 split into 4 byte planes and compressed with zstd level 3 (lossless; the
planes compress much better than raw floats). The protocol keeps a per-tile encoding field, so a float16 mode can
be added in M9 if measured bandwidth needs it.

**D-054 Network tests run processes with `penumbra-nettest`.** CMake cannot run several processes at once, so a
small test program (`tests/net/nettest.cpp`) starts the controller and workers, can hard-kill one after a delay,
collects each process's output, and checks every exit code. Each network test uses its own port (7501+) so
`ctest -j` can run them side by side.

**D-055 Worker device settings stay local.** A worker renders with the controller's settings except
`--backend`, `--devices`, `--threads`, `--vram-limit`, `--ram-limit` and `--no-texture-downsize`, which come from
the worker's own command line and config file. A worker that cannot connect because nothing listens yet keeps
trying for 15 s (the controller may still be starting), then exits with code 7.

**D-056 `DeviceFailed` message.** The guide's table has no message for "one of my devices failed". Version 1 adds
`DeviceFailed` (device id, error) next to `Release`, so a remote device failure is logged and counted exactly like
a local one.

**D-057 M4 step 1 loads the scene from the controller's paths.** Until scene packages exist (step 3), a
`SceneOffer` carries absolute paths and the worker loads the files from them, which works on one machine or a
shared folder. Connections are not encrypted until step 2; the README says so.

**D-058 TLS 1.3 with mbedTLS works; no Noise fallback.** A spike (unit tests in `tests/unit/test_net.cpp`)
confirmed mbedTLS 3.6.5 completes a TLS 1.3 external-PSK handshake and refuses a different key. Details: key =
HKDF-SHA256(16 canonical code characters, no salt, info `penumbra-join-v1`), identity `penumbra-join-v1`, only
the `psk_dhe_ke` mode (ECDHE for forward secrecy), AES-256-GCM / ChaCha20-Poly1305 / AES-128-GCM. TLS runs on
memory buffers under the existing Asio connection, so all socket work stays asynchronous on one network thread.
A handshake must finish within 10 s.

**D-059 Join code storage and checks.** The controller's code lives in `<user data>/join_code.txt` (created on
first `--host`; delete it to regenerate until the UI has a Regenerate button). `--code` / `PENUMBRA_CODE` overrides
it. More than 5 failed handshakes from one address within a minute block it for 10 minutes. A worker whose
handshake fails exits with code 6 (rejected).

**D-060 `--approve-workers` in the terminal** asks `Accept worker '<name>' from <address>? [y/N]` on stderr and
reads stdin, one worker at a time; end of input or any other answer rejects (`NotApproved`). The UI asks with a
dialog from M10.

**D-061 Test hook `PENUMBRA_TEST_GIT_HASH`** (test-only like D-047, logged as a warning): a worker claims that git
hash in its Hello, so the version-mismatch rejection is tested without building a second binary.

**D-062 Scene packages leave the settings out.** The guide puts `RenderSettings` in the package manifest, which
would make every change of spp or resolution a new package and a new download. The manifest describes only the
files (paths relative to the deepest folder holding them all, sizes, file hashes, 4 MB chunk hashes), and the
settings travel in each `SceneOffer`. The worker points the scene and HDRI paths into its cached copy. The package
is not written to disk on the controller: chunks are read from the original files when sent. Files on different
Windows drives cannot be packaged (Penumbra says so); glTF files keep their relative URIs, so no rewriting is
needed.

**D-063 Scene file discovery is explicit.** `collectSceneFiles` lists what the loaders read: the scene file, glTF
buffers and images (external URIs), OBJ `mtllib` files and their `map_Kd` textures (the only map the OBJ loader
uses), and the HDRI. Packaging runs for every render, so a missing file fails before rendering, with its name.

**D-064 Scene cache and admission.** Cache folder per package hash, a `<hash>.partial` file for downloads in
progress (verified chunks are kept, so a download resumes at the first missing chunk), least recently used
packages evicted above `--cache-size`. A worker refuses a scene bigger than its cache, needing more than its free
disk (twice the package: download plus unpacked copy), or more than its available RAM, and answers
`SceneHave Rejected` with the reason; the render continues without it. Test hook `PENUMBRA_TEST_FREE_DISK`
(test-only, logged) fakes the free disk space.

**D-065 Protocol version 2** adds `SceneHave`, `SceneChunk` and the package fields of `SceneOffer`. The uploader
keeps at most 2 chunks queued per connection (so acks and leases stay prompt), serves several workers
round-robin, and counts `--upload-limit` in bytes on the wire.

**D-066 Test hook `PENUMBRA_TEST_EXIT_AFTER_CHUNKS=N`** (test-only, logged): a worker exits abruptly with code 137
right after its N-th scene chunk, so the resumed-download test does not depend on timing; `penumbra-nettest`
can start a process once another has ended (`after:NAME`).

**D-067 More test fault modes.** `PENUMBRA_TEST_FAULTS` (D-047) gains `mode=exit` (the process exits with 137 after
K items: a crashed worker holding leases), `mode=freeze` (the device hangs and a network worker stops sending
anything, heartbeats included: a dead link) and `mode=corrupt` (a network worker damages tiles after computing
their checksums). Every fault-table row is a network test that must end bit-identical to the solo render.

**D-068 Heartbeats: the worker asks, the controller echoes.** The worker sends `Heartbeat` every 5 s and the
controller answers at once, so the answer is both the controller's heartbeat and a round-trip measurement (the
guide's "3 items in flight above 50 ms" uses it). Both sides drop a connection with nothing received for 30 s; the
controller checks every second and requeues the dropped worker's leases. A worker with 3 rejected results is sent
`Kick` and disconnected.

**D-069 `penumbra-netem-proxy` instead of OS traffic shaping.** Linux `tc netem` needs root and is missing on
Windows, so the bad-network test runs through a small TCP proxy in `tools/`: a fixed delay each way, a bandwidth
cap per direction, and "loss" as a 200 ms stall of a block (and the blocks behind it), which is what a lost TCP
segment costs; a proxy cannot drop segments inside a TCP stream.

**D-070 `--stay` and resuming a hosted render.** A `--stay` worker keeps its connection after `RenderComplete` and
waits for the next `SceneOffer`; when the connection drops (or the controller is not up yet) it reconnects after 1,
2, 4 ... 30 s, forever. A wrong code, another version or a kick still end it (exit code 6): retrying cannot fix
those. A hosted render records `hosting` and its join code in the session file, so `penumbra --terminal --resume
<session.json>` hosts again on the same port with the same code; the workers rejoin and download nothing (scene
cache).

**D-071 Scale test and `PENUMBRA_TEST_NULL_DELAY_MS`.** The guide's scale test (64 workers x 8 devices) runs as a
normal network test: 64 worker processes with 8 null devices each on loopback. The null backend is instant, so a
test-only delay (it only affects the test-only null backend) makes each item take 300 ms and keeps all 512 slots busy for
the whole render. First result on the laptop: 64 workers joined, 8,640 items in 39 s, bit-identical.
`penumbra-nettest` reads its process list from a file (`--procs`) because 64 command lines exceed the Windows
command-line limit.

**D-072 UPnP and the address list.** A headless `--host` controller asks the router (miniupnpc, 2 s discovery on a
background thread) to forward its port, unless `--no-upnp` or it listens on loopback only; the mapping has no
expiry and is removed when the render ends. Every outcome is logged: forwarded (with the public address for
`--join`), no UPnP router, refused, or a router whose own internet address is private or in 100.64.0.0/10
(carrier-grade NAT or a second router: use a VPN). On start the controller also lists its LAN and VPN addresses
(Tailscale 100.64.0.0/10 and fd7a:115c:a1e0::/48, WireGuard/Tailscale adapters) with the full join command.
Network tests pass `--no-upnp`. Checked on the laptop: the home router forwarded and later removed TCP 7420.

**D-073 Bit-identical per platform build; across Windows and Linux within 1e-10.** Cross-OS runs on the laptop
(Windows controller with two WSL Ubuntu workers, and a WSL controller with two Windows workers, same git commit,
spheres scene at 720p / 64 spp) match the controller's solo render to relMSE 9.3e-11 (max abs diff 1.5e-3 on HDR
values up to ~300), with about 3% of float values differing in the last bits. MSVC and GCC builds use different
system math libraries (sin, cos, pow, exp) and build Embree with different compilers, so a tile rendered on Linux
is not bit-identical to the same tile rendered on Windows. That is far inside the guide's tolerance for any
distribution of work (relMSE < 1e-6) and invisible. D-045 still holds among hosts of one OS (any number of
devices, workers, failures, resume). Making Windows and Linux bit-identical would need Penumbra's own math
functions everywhere; not worth it, since GPU backends (M5/M6) differ by up to 1e-3 anyway. Two sources, measured: the
renderer calls the math library (sampling warps, HDRI directions, glossy lobes), so even the Cornell box differs
(Windows vs Linux solo: relMSE 6e-16); and `penumbra-scenegen` writes slightly different `spheres.gltf` files on
each OS (sphere vertices use sin/cos), while `cornell.gltf` and `sky.hdr` are identical. The network path itself
is exact: a Windows controller merging a Linux worker's Cornell tiles gives the Linux solo render bit for bit.

**D-074 Internet worker over Tailscale in a container without a TUN device.** The vast.ai container has no
`/dev/net/tun`, so `tailscaled` runs with `--tun=userspace-networking --outbound-http-proxy-listen=localhost:1055`,
and `socat TCP-LISTEN:7420,fork,bind=127.0.0.1 PROXY:127.0.0.1:<laptop tailscale ip>:7420,proxyport=1055` lets the
worker join `127.0.0.1:7420`; the bytes cross the internet inside Tailscale. Penumbra needs no proxy support.
Results (2026-10-19, laptop controller on Windows, server Ubuntu 24.04 with 80 cores): the server joined from its
Tailscale address, downloaded the package, rendered spheres at 1080p / 128 spp (68 items) in 22 s, and the image
equals the server's own solo render of the cached package files bit for bit. Mixed render: laptop CPU + WSL worker
over the LAN + server over the internet, Cornell 1080p / 256 spp: 8 + 12 + 108 items, relMSE 8.5e-11 against the
Windows solo render (Linux tiles, D-073). UPnP on the home router forwarded and removed TCP 7420; an internet join
through the public address was not tested (the user chose the VPN path).

**D-075 M4 gate results (2026-10-19).** Windows (laptop, MSVC 19.51): 154/154 tests, also from a fresh clone of
`m4` built with `scripts\build.ps1` per the README. Linux: 155/155 in WSL Ubuntu 22.04 (GCC 12.3) and 155/155 on the
vast.ai server (Ubuntu 24.04, GCC 12.4) after following the README's Linux steps on a fresh machine. Loopback: a
controller and 3 worker processes give the solo render bit for bit, also with failed devices, crashed, hung,
silent and corrupt workers, a worker joining late, a 200 ms / 2% loss / 10 Mbit/s link, a killed and resumed
controller with --stay workers, and 64 workers x 8 devices. LAN: Windows controller with WSL workers and the
reverse (D-073). Internet: the server over Tailscale, bit-identical to its own render of the package (D-074).
Not covered: an internet join through a forwarded public port (UPnP mapping itself checked), the UI (M10).

## M5: Vulkan RT backend

User approved the M5 plan on 2026-10-20 and decided to close M5 without a Windows GPU run for now (Linux GPU on a
rented vast.ai container plus the Windows build).

**D-076 No Vulkan SDK needed.** vcpkg supplies the Vulkan headers, volk (runtime loading, so Penumbra starts on
machines without a Vulkan driver), Vulkan Memory Allocator and a pinned Slang release (2026.19, used as a host
build tool: `slangc` compiles the shaders to SPIR-V at build time and they are embedded in the binary). Users only
need a GPU driver; the Vulkan SDK (validation layers) is optional, for development.

**D-077 Shared shading code: C++ stays the reference, Slang mirrors it for the GPU.** Trial (Slang 2026.19): the
shading math compiles to SPIR-V and to C++ alike, but Slang's C++ target refuses ray tracing ("TraceRayInline not
available for cpp"), so a Slang CPU path would split every bounce between generated code and Embree glue and
change the CPU golden image. The CPU renderer keeps its C++ code (`core/shading`, `core/render`, already written
for CUDA too via PEN_HD, for M6); `shaders/*.slang` is a line-by-line port for Vulkan, and a GPU parity test
evaluates every sampling, BSDF and light function on the same random inputs on both sides, so drift shows up per
function instead of as a slightly wrong image.

**D-078 One ray-tracing pipeline, path loop in the ray generation shader (megakernel).** `shaders/pathtrace.slang`
runs the whole path (NEE, MIS, Russian roulette) per pixel and sample in raygen; one hit group (closest hit
reports the hit, any-hit applies alpha cutouts) and one miss shader serve camera, bounce and shadow rays (shadow
rays skip the closest hit and stop at the first hit). Geometry without alpha is marked opaque, so any-hit only
runs where a material uses alpha. Each BLAS holds a mesh's primitives in order, so `GeometryIndex()` is the
primitive and `InstanceID()` (custom index) the scene instance; meshes without triangles stay out of the TLAS.
Backface culling is off, as in Embree. M9 compares this with a wavefront design.

**D-079 GPU scene data in plain storage buffers (scalar layout).** Vertex attributes, materials, textures (RGBA8
packed in a uint, or float4), alias tables, emissive triangles and the HDRI are buffers whose C++ structs
(`backends/vulkan/gpu_types.h`) and Slang structs (`shaders/types.slang`) match byte for byte. Textures are
filtered in shader code exactly like the CPU (same wrap modes, same sRGB table), not by the texture units, so the
GPU reads the same texel values; M8's "textures in host memory" tier only has to move these buffers. The sheen
albedo and sRGB tables are computed on the CPU and uploaded. Memory plan in M5: tier 0 (everything in VRAM) when
the estimate fits in 90% of VRAM, otherwise the GPU is skipped with a message (tiers in M8).

**D-080 Test setup for GPUs.** Render and network tests run on the CPU unless they name a backend
(`--backend cpu` is added), so they mean the same on machines with GPUs. GPU tests carry the ctest label `gpu`; the
build scripts skip it (`-LE "large|gpu"`), and `scripts/test-gpu.sh` / `.ps1` run them. `penumbra-gpu-parity`
evaluates 13 shading functions on 65,536 hash-derived inputs on the GPU and in C++: integer paths must match bit
for bit, floats within a relative 2e-3 in 99.5% of cases (the rest are ill-conditioned inputs).

**D-081 CPU threads follow the container's CPU share.** `std::thread::hardware_concurrency()` reports every host
CPU inside a container: on the vast.ai GPU container it said 192 while the rental grants 48, so a CPU render
started 190 threads and crawled. `usableCpuThreads()` takes the minimum of the hardware count, the affinity mask
and the cgroup CPU quota (v2 `cpu.max`, v1 `cfs_quota_us`); the CPU backend, the device manager, the OpenEXR
writer threads and the Hello/Stats CPU count use it.

**D-082 M5 gate results (2026-10-20).** Linux GPU (vast.ai container, Ubuntu 24.04, GCC 12.4, NVIDIA RTX 5060 Ti
16 GB, driver 590.48): 155/155 tests and 10/10 GPU tests on commit 6963ec2, built from a fresh clone per the
README. Vulkan vs CPU at 4,096 spp: golden Cornell relMSE 1.1e-8, spheres 7.9e-10 (gate: 1e-3); big texture
2.2e-9 at 1,024 spp; mixed GPU + CPU render 1.1e-9; furnace uniform; Vulkan repeats bit for bit; parity of 13
shading functions passes (integer paths exact). Speed: about 15x the 48-core Xeon (docs/BENCHMARKS.md).
Windows (laptop, MSVC 19.51, Intel UHD without ray tracing): 154/154 with the GPU skipped, also from a fresh clone
of `m5`. Not covered, by the user's decision: a Windows GPU run (scripts\test-gpu.ps1 on the gaming PC, later).

## After M5: work items for fast GPUs on slow links

User test 2026-10-20: laptop controller, RTX 5060 Ti worker joining over Tailscale (userspace mode, 141 ms round
trip). The GPU sat at 5% and the render ran at well under half its speed: an item was 2^22 pixel-samples (17 ms on
this GPU), each item sends a full tile back, and 3 items in flight cannot cover a 141 ms round trip. The user asked
to keep results bit-identical, which rules out float16 tiles (D-053).

**D-083 Work items of 2^26 pixel-samples (was 2^22).** Chunks stay fixed by the settings alone, so every
distribution of work still gives the same bits (D-045); only the last bits of images compared with earlier
versions change once (golden tests unaffected). One item is about 0.3 s on an RTX 5060 Ti and a few seconds on a
many-core CPU (the guide's 2-10 s), and the bytes sent per sample drop 16x. A 256 x 256 tile now takes chunks of
1,024 samples; frames below 1080p (one tile) get correspondingly more samples per chunk. Costs: coarser preview
steps and more work lost when a render is cut. Test-only `PENUMBRA_TEST_CHUNK_BUDGET` restores small items for the
tests that need many items per tile (slots, faults, resume, interrupt, all network tests, unit tests).

**D-084 Items in flight follow the round trip.** A network worker keeps ceil(2 x round trip / item time) + 1 items
per device in flight (2 to 64; the device's median of its last 16 items), so a fast device behind a long link is
never idle waiting for its next lease. It measures the round trip as soon as it joins. The guide's "2, or 3 above
50 ms" remains the rule until the first items have finished. Small changes are ignored to avoid fluttering.

## M6: CUDA compute backend

**D-085 One GPU path tracer, two ways to trace.** The path loop moved from `shaders/pathtrace.slang` into
`shaders/integrator.slang`, written against two functions the entry file defines before including it:
`intersect(o, d)` (closest hit) and `occluded(o, d, tmax)`. `pathtrace.slang` implements them with Vulkan ray
tracing (`TraceRay`), `pathtrace_cuda.slang` with the software BVH (`bvh.slang`). slangc compiles the same shading
code to SPIR-V and to CUDA C++ (the guide's plan); the C++ code stays the reference (D-077) and the parity test now
runs on both GPU backends. A work item may be split into several CUDA launches of about 1M paths each (sample
ranges summed in order, the running sum kept in the output buffer), so each launch is short and the result is the
same bits as a single launch.

**D-086 Software ray tracing: tinybvh CWBVH, one byte layout for both GPU backends.** tinybvh is kept in
`third_party/tinybvh` (MIT; it has no vcpkg port), pinned to commit bfd3e819 (v1.9.0, 2026-09-29), compiled in one
file with its warnings off; its fatal errors throw `BackendError` instead of ending the process. Each mesh gets a
CWBVH (`BVH8_CWBVH`, Ylitie et al. 2017); a mesh of 1 to 3 triangles is a plain triangle list, because tinybvh
cannot make a CWBVH whose root is a leaf. A binary BVH over the instances (median splits, 2 per leaf) sits on top;
rays enter each mesh in object space with an unnormalized direction, so `t` stays the world ray parameter. The
traversal is a port of tinybvh's `traverse_cwbvh.cl`, with these changes:
- Alpha cutouts are tested inside the loop.
- Shadow rays stop at the first hit.
- Directions with zero components use a clamped reciprocal (orthographic cameras send exactly axis-parallel rays).
- The triangle test cannot accept NaNs.
- A full stack is counted and reported, not ignored.

The node array is tinybvh's, with one change: each node's leaf triangle base becomes a triangle number. The
triangles are Penumbra's own (`v2 - v0`, `v1 - v0`, `v0` with a scene-wide triangle id), rebuilt from the mesh in
tinybvh's order. The pinned header turns on `CWBVH_COMPRESSED_TRIS` (4 float4 per triangle, a precomputed transform),
and the first CUDA renders read that array as 3 float4 per triangle and crashed with an illegal address. Our format
is the same whichever layout a later tinybvh picks, and the Moller-Trumbore test matches what the reference kernel
does without compression.

Slang's CUDA prelude (2026.19) gives `firstbithigh` on a uint with bit 31 set the signed meaning, which breaks the
CWBVH hit masks; `highestBit()` avoids it. The GPU structs (`backends/gpu/gpu_types.h`, `shaders/types.slang`)
are ordered so Vulkan's scalar layout and CUDA's natural alignment give the same bytes: float2 on 8 bytes, float4 on
16, sizes padded by hand. Only `TextureRef` and `Material` were reordered. The build turns slangc's reflection JSON
into a header (`cmake/CudaLayout.cmake`); the CUDA backend fills the kernel's parameter block by name from it and
static_asserts the offset of every field of every struct.

**D-087 CUDA kernels are built ahead of time; the driver is loaded at run time.** nvcc compiles the generated CUDA
C++ into a fatbin embedded in the binary: machine code for every architecture in `PEN_CUDA_ARCHS` that the toolkit
supports (the others are left out with a CMake warning), all produced from ONE PTX of the oldest architecture,
which is embedded too. Every GPU generation therefore starts from the same program, and the driver can compile
the PTX for GPUs newer than the list. Results agree very closely across generations but are not guaranteed bit for
bit. On the RTX 5070 Ti and the GTX 1660 SUPER the Cornell box gave identical bits, while the spheres scene
differed in the last bits, because ptxas schedules each architecture differently. Nothing links against CUDA: `nvcuda.dll` / `libcuda.so.1` is opened at start,
so a machine without NVIDIA still runs Penumbra, and `--diagnose` says why CUDA is missing. A GPU is usable when
the build has machine code for its architecture (same major version, equal or lower minor) or it is at least the
PTX architecture, and the driver supports the toolkit's major version (CUDA 13 kernels need a CUDA 13 driver).
CUDA 13 dropped compute capability 6.x and 7.0 (GTX 10-series, V100): building for those takes a CUDA 12.x
toolkit and `-DPEN_CUDA_ARCHS="61;75;..."`. `PEN_ENABLE_CUDA` is ON when CMake finds a CUDA toolkit
(`find_package(CUDAToolkit)`). On Windows nvcc gets `-allow-unsupported-compiler`, because it only preprocesses
with MSVC here (device code only).

**D-088 One GPU list for both backends.** The `--devices` numbering lists GPUs with Vulkan ray tracing in Vulkan
order, then NVIDIA GPUs only CUDA can use, in CUDA order. A GPU on both lists is matched by UUID and appears
once. `--backend auto` picks Vulkan where the GPU has ray tracing, else CUDA, and logs the choice per GPU.
`--backend cuda` forces CUDA (also on RT GPUs, for testing), and `--backend vulkan` skips GPUs without RT. Tested
on a GTX 1660 SUPER (2026-10-22): its driver (610) offers Vulkan ray tracing on the GTX 16 series, emulated on the
shader cores, so `auto` uses Vulkan there too. CUDA is the default only where the driver offers no Vulkan RT
(GTX 10 series and older, which need a CUDA 12 build, D-087), or where Vulkan itself is missing.
`--integrator debug:bvh` (D-027) now works with `--backend cuda`: each pixel holds the number of BVH nodes visited
plus triangles tested by the camera ray. Without `--backend cuda` the render is refused before it starts. The CPU
and Vulkan backends refuse it in `prepare()`, so a worker that joins with another backend fails its devices
instead of rendering zeros.

**D-089 M6 gate results (2026-10-22).** Linux, vast.ai containers, Ubuntu 24.04, GCC 12.4, CUDA 13.3, driver 610,
each built from a fresh clone per the README:
- **RTX 5070 Ti 16 GB** (Blackwell, sm_120, 30 CPUs): full suite passes; 24/24 GPU tests (Vulkan and CUDA).
  CUDA vs CPU at 4,096 spp: golden Cornell relMSE 1.1e-8, spheres 1.4e-9 (gate: 1e-3). Big texture 8e-10; cutout
  (alpha, instancing, orthographic) 5.8e-10. CUDA vs Vulkan 2.2e-9. Parity of every shading function passes.
- **GTX 1660 SUPER 6 GB** (Turing, sm_75, the oldest default target, no RT cores; 14 CPUs): full suite 157/157;
  13/13 CUDA tests, with the same Cornell relMSE (1.082e-8) and spheres 1.4e-9.

Both GPUs: CUDA repeats bit for bit; a mixed CUDA + CPU render stays within the tolerance, with both devices
rendering items; `debug:bvh` renders.

Windows (laptop, MSVC 19.51, no CUDA toolkit, so CUDA is not built): full suite passes. The `chunks` change
(D-083, D-084) is tested in the same runs. They found two test harness bugs and one logging gap, all fixed:
- `cmake -E env` wrappers orphaned killed processes.
- One resume test was too fast on fast machines.
- The first in-flight depth was not logged.

Also from these runs:
- The hybrid GPU tests had silently become CPU-only once items grew to 2^26 pixel-samples; they now use small
  chunks and check that both devices rendered.
- CUDA is about 9x slower than Vulkan RT on an RTX GPU (docs/BENCHMARKS.md), which is for M9 to tune.

Not covered: CUDA on Windows (no toolkit on the laptop; the user's call), a GTX 10-series GPU (needs a CUDA 12
build, D-087), and a Windows GPU run (gaming PC, later).

**D-090 `--devices` items name their backend (M7, 2026-10-22).** A GPU item in `--devices` may carry `:vulkan` or
`:cuda` (`0:vulkan,1:cuda,cpu`), so one host mixes backends; without a suffix the GPU follows `--backend`, and
`auto` picks Vulkan RT, else CUDA (D-088). The same GPU may be listed twice (`0:vulkan,0:cuda`), as two
independent slots; this is for tests, not speed. Measured on an RTX 3060 (2026-10-22): CUDA alone 12.2 M
samples/s; Vulkan + CUDA on the same GPU 13.1 M samples/s together, with the driver time-slicing the two contexts so
unevenly that the CUDA slot finished 1 item while Vulkan did 31. The image is right; nothing is gained. A suffix that contradicts a non-auto `--backend` is refused before
the render. Hybrid CPU: when GPUs render too and `--threads` is not set, the CPU device leaves one thread per GPU
slot (plus 2 on the controller host) so the GPU feeder threads are not starved; the log says how many it took.

**D-091 Tail duplication (M7).** When the scheduler has nothing left to hand out and a device that wants work has
no lease at all, the controller gives it a copy of the oldest outstanding lease of another device, if that device
should finish sooner: the lease is older than the owner's mean item time, or the idle device's mean item time is
below what the owner has left (its mean minus the lease's age); devices without a finished item yet always qualify.
The check runs on every event and every 200 ms, as leases age. Each item gets one copy at most; late-reissued leases and copies are not
copied again. The first valid result is merged; the other is acknowledged as `duplicate` and dropped. The loser is
not cancelled (workers only cancel everything, protocol 1-3): it finishes or is cancelled at the end of the render,
which comes when every chunk is merged, so it never delays the image. A lost lease (device failed, released,
worker gone, invalid result) is only requeued when no other lease still holds its item. Same backend means the
same bits, so the image does not depend on which copy wins; a copy on another backend stays within the usual
cross-backend tolerance. The 10x-median late-lease check (D-049) stays for hangs while the queue still has work;
its tests turn duplication off with the test hook `PENUMBRA_TEST_NO_TAIL_DUPLICATION=1` (D-047, logged when active).
The summary logs how many items were copied and how many copies won.

**D-092 Devices prepare in parallel; a device that cannot take the scene is skipped (M7, protocol 3).** Every
device plans memory, uploads the scene and prepares the frame on its own thread. A device that throws is reported
in `WorkerReady` as `skipped` with the reason (new fields in `DeviceReadyDesc`, protocol version 3) and keeps its
slot number, but never starts and gets no lease; the controller logs a warning and counts only the others. Only
when every device fails does the worker answer `scene_rejected` (the local worker throws `BackendError` with all
reasons).

**D-093 NVML readings, loaded at run time (M7).** Worker `Stats` report per-GPU usage, VRAM used/total and
temperature from NVML (`nvml.dll` / `libnvidia-ml.so.1`, part of the NVIDIA driver), opened with
LoadLibrary/dlopen like the CUDA driver (D-087), so no SDK or header is needed and machines without it run the
same. The few entry points are declared in `backends/gpu/nvml.cpp`. A GPU is found by its UUID
(`nvmlDeviceGetHandleByUUID`, the same UUID Vulkan and CUDA report, now in `DeviceInfo`). Without NVML, or on
another vendor's GPU, usage falls back to Penumbra's busy % and temperature is "unknown" (< -273), as the
schema already allowed: no protocol change. `--list-devices` shows the live VRAM use, usage and temperature too.

**D-094 A hung GPU cannot stop the render (M7).** The Vulkan fence wait (in 100 ms slices) and the CUDA wait
(`cuStreamQuery` polling instead of `cuCtxSynchronize`) watch the cancel flag. A job that still runs 10 s after a
cancel marks the device lost: the slot fails with a `BackendError` and the backend's destructor skips waiting and
freeing (it leaks rather than hang or free memory the GPU may still use). Errors that already end in an error code
(Vulkan `VK_ERROR_DEVICE_LOST`, CUDA sticky errors such as an illegal address) fail the item, the device is
removed (`DeviceFailed`) and its leases go to the others, as for any device failure (D-047 fault tests). While the
render runs, a hung device's leases are caught by tail duplication (D-091) or the late-lease check (D-049); the
cancel at the end of the render then frees its slot within 10 s.

**D-095 Resume tests cut the render by merged chunks, not by time (M7).** `render.resume_chunks` failed on the
4x RTX 3060 box: one thread of its shared EPYC renders a chunk in about 14 s, so the run killed after 15 s had no
chunk in its only checkpoint. A fixed kill time cannot suit both fast and slow machines, so the resume tests set
the test hook `PENUMBRA_TEST_KILL_AFTER_ITEMS=N` (D-047 style, logged when active): once N chunks are merged
the controller checkpoints the film and session and exits at once with code 137, like a killed process. The
resume then has to reproduce the uninterrupted image bit for bit, as before.
Found in the first cross-machine render (4x RTX 3060 host, laptop worker): Vulkan rendered a whole item (2^26
pixel-samples) in one dispatch, 4-7 s on that host, so two healthy GPUs still busy with tail copies at the end of
the render passed the 10 s grace and were removed as hung, and the render ended 11 s late. Vulkan now splits an item
into dispatches of at most 2^22 paths (one submit each, cancel checked between them), the same scheme as CUDA's
launches (`kItemAccumulate`; the same bits as one dispatch), so a cancel takes effect within one short dispatch.

**D-096 Sample scenes in the repository (M7, user request).** `scenes/` holds the `cornell`, `spheres` (with
`sky.hdr`) and `cutout` scenes, about 2.2 MB, so a fresh clone can render right after building (README "Quick test
right after cloning"). They are exactly what `penumbra-scenegen` writes: `.gitattributes` keeps them byte-exact on
every OS (no CRLF conversion, so the scene package hash is the same everywhere), and the `docs.sample_scene_*`
tests compare them with the generated copies, except `spheres.gltf`: its vertices come from sin/cos, whose last bits
differ between MSVC and GCC (found on the RTX 5080 gate box), so the generator's output is not byte-identical across
OSes. The `render.sample_*` tests render each committed scene instead. The furnace and big-texture scenes
(8-12 MB, test-only) stay generated.

Also from the 5080 box: with tail duplication (D-091) a fast host finishes a frozen or slow device's last items, so
`net.worker_silent` (which waits for the 30 s heartbeat loss) and the hybrid/mixed tests that require every device to
win items now run with `NO_TAIL=1`, like the late-lease tests.

**D-097 Late leases are judged per device (M7).** The late-lease check (D-049) used one median item time for the whole
render. In the LAN + internet cross-machine test (laptop controller, a CPU worker on the LAN, 2x RTX 5080 over
Tailscale) that median was the GPUs' ~0.3 s, so every CPU item (~25 s) was declared late after 3-8 s and handed to a
GPU, and the CPU worker never merged anything. A lease is now late after 10x the median of its own device's recent
items (the last 32, duplicates included); a device with fewer than 3 finished items is judged by the median of all
devices, so a device that hangs from its first items is still caught. Slow devices then contribute, and hung ones
are still reissued.

**D-098 M7 gate results (2026-10-22 and 2026-10-23).** Gate: "a multi-GPU worker matches 1 GPU; a removed GPU never
stops the render".
- **4x RTX 3060 12 GB** (vast.ai, Ubuntu 24.04, driver 580, 82-CPU quota), 2026-10-22: full suite 162/162 and GPU
  tests 32/32 (Vulkan 17/17 again after the dispatch split, D-094). 4 GPUs give the same bits as 1 GPU per backend; a
  GPU that throws or hangs mid-render leaves the image unchanged; 4 GPUs render 3.95x one GPU (docs/BENCHMARKS.md).
- **2x RTX 5080 16 GB** (vast.ai, Ubuntu 24.04, driver 595, 23-CPU quota), 2026-10-23: README setup from an empty
  container, fresh clone, build, full suite. The first run found four test-harness problems, all fixed in tests
  only (D-096, D-091 notes, `net.worker_silent` timing); then 172/172 and GPU tests 32/32 at 7f342c7.
- **Windows** (laptop, MSVC 19.51, no CUDA toolkit): fresh clone in `C:\dev\pen-m7` built with `scripts/build.ps1`,
  168/168. **Ubuntu 22.04** (WSL, GCC 12.3): fresh clone, `scripts/build.sh`, 168/169; the only failure was the
  `spheres.gltf` byte check, since removed (D-096). Later commits changed tests, docs and two controller functions
  (D-097), which were rerun on both.
- **Cross-machine** (guide "Cross-machine tests before closing M4 and M7"), all over Tailscale and checked against a
  render on one machine:
  - Linux controller (4x 3060), Windows worker (laptop): relMSE 3.9e-11.
  - Windows controller (laptop), Linux worker (4x 3060) through the container relay: relMSE 7.9e-11.
  - A LAN worker and an internet worker in one render: laptop controller (`--no-local`), a worker in WSL Ubuntu
    22.04 joining the laptop's LAN address 192.168.2.129, and the 2x RTX 5080 box joining over Tailscale with the
    README's container relay; 4K, 8,192 spp: relMSE 2.3e-13. The LAN worker merged 6 items once late leases were
    judged per device (D-097). The LAN worker ran on the same laptop (WSL2, its own virtual network), not on a
    second physical machine; the user had none on the LAN at the time.
- Found and fixed on the way: Vulkan items as one long dispatch made slow GPUs look hung (D-094); timing-based
  resume tests (D-095); one render-wide late-lease median starved slow devices (D-097); a misleading "wrong join
  code" error when a relay dropped the connection.

Open: the 3060 host's low absolute speed (about 9 M samples/s per GPU at about 30 W, both backends) to recheck in
M9; on one GPU listed twice the driver time-slices the two contexts unevenly (D-090).

**D-099 M10 (UI) before M8 and M9 (user decision, 2026-10-23).** The user asked to build the UI next and do M8
(memory budget) and M9 (performance) after it. The UI drives the existing controller and worker code, which M8 and
M9 change only inside devices, so nothing blocks it. Until M8, the UI shows each GPU's memory tier as reported
(always tier 0 today), and the `--vram-limit` / texture-downsize settings are added to the UI together with the
flags in M8. Until M9, render-time estimates use the speed the cluster measured in the session, not `--benchmark`.

**D-100 M10 UI choices (user, 2026-10-23).** Dear ImGui on GLFW (guide), Vulkan swapchain with an OpenGL 3.3 fallback.
- Charts (CPU/RAM history, throughput sparkline) are drawn with ImGui's draw lists: no ImPlot.
- IBM Plex Sans and Mono (SIL Open Font License 1.1) are bundled in `third_party/fonts/` with their license and
  compiled into the binary, so the UI looks like the mockups on every machine.
- Open-file dialogs: the user approved `nativefiledialog-extended`, but on Linux it links GTK or D-Bus, and a headless
  worker (same binary) would then fail to start on a server without them. So no library: on Windows the system
  `IFileOpenDialog`, on Linux `zenity` or `kdialog` when installed, else a file browser inside the app.
- The 3-machine gate runs one machine's UI on a rented GPU container under a virtual display (Xvfb + a VNC server
  with noVNC), which the user opens in a browser through the SSH tunnel on port 8080. That container's UI uses the
  OpenGL fallback (no GPU for the display), while it renders with its GPUs as usual.
- `--terminal`, `--worker` and every info mode never load window libraries (GLFW and ImGui's OpenGL loader open
  them at run time only in UI mode), so workers keep starting on servers with no display libraries at all.
- GLFW 3.4 is vendored in `third_party/glfw` too (zlib license), built with X11 only: the vcpkg glfw3 port builds the
  whole X11 client library chain from source on Linux (autotools included), which failed on a GNU mirror timeout
  in WSL and would slow every container build. GLFW needs only the system X11 headers (`libx11-dev libxrandr-dev
  libxinerama-dev libxcursor-dev libxi-dev`) and opens libX11 at run time; `ldd penumbra` lists no display
  library. Wayland desktops run the UI through XWayland. On Linux, `build.sh` builds the UI only when a display is
  present (`--ui` / `--no-ui` to force). Checked: Windows draws with Vulkan (Intel UHD), WSL falls back to OpenGL
  3.3 on its own (no Vulkan loader there).

**D-101 How the UI is built (M10, steps 2-4).**
- **One render path.** The UI and `--terminal` share `controller/render_job` (prepare: settings, scene package,
  scene, disk check, film and session file; finish: output, summary, cleanup) and `HostingSession` (one host and
  join code across renders, workers stay attached between renders, one render at a time through RenderController).
  The UI's `RenderRunner` runs exactly that on a background thread; Stop is Ctrl+C (checkpoint, image written).
- **Protocol 4** adds the round trip, upload rate and scene cache use to `Stats`, for the Hosts pages' Ping, Upload
  and Scene cache; worker `Log` lines are kept per host for "Log from this host".
- **Switching hosts or devices off** (Hosts pages) is done in the session: a disabled device's work requests are
  dropped and a disabled host is not offered renders, so RenderController is unchanged. This machine's disabled
  GPUs are left out of its `--devices` list. Applies to renders that start afterwards.
- **Framing preview** (Render page): the guide's `renderPreview`, done as a direct small render on this machine's
  CPU (a few samples, about 480 px wide), not through the scheduler: it is a UI aid and never part of an image.
- **Textures** (preview, viewport) are ImGui 1.92 `ImTextureData` registered as user textures, so the Vulkan and
  OpenGL backends upload them; `ImTextureData::GetTexRef()` must be used (`ImTextureRef(ptr)` is the legacy
  `void*` ID constructor and draws the wrong texture).
- **Test hooks** for checking pages without clicking (D-047 style, logged when active): `PENUMBRA_TEST_UI_PAGE`
  opens a page at start, `PENUMBRA_TEST_UI_SCREENSHOT=<png>` saves the frame shown after 8 s (OpenGL) and exits.

**D-102 Viewport, Settings and Join a render (M10, steps 5-6).**
- **Live view.** RenderController keeps a downsampled copy of the film (longest side 2,048 px, box filter,
  weighted by sample count) updated as tiles merge, so the Viewport never reads a gigapixel film to draw. Zooming
  past it reads the visible region at full resolution from the film (`readFilm`). Tile colours are the host that
  merged the tile (8 colours, hatched beyond); outlined tiles are leased right now.
- **Pause** stops handing out leases (leases out finish and merge); Resume restarts the lease clocks so paused
  time never counts as a late lease. "Save image now" writes the output from the film as it is.
- **Join a render** runs the ordinary worker (`runWorker`, `--stay` behaviour) on a background thread with a
  `WorkerStatus` the worker fills in for the worker screen. Hosting stops first (one machine is a controller or a
  worker, never both) and starts again after Leave. Leave is the worker's Ctrl+C: leases are handed back.
- **Hosting switches.** Internet hosting is the UPnP mapping switched on and off on the running host
  (`HostingSession::setInternet`); it is off at every UI start whatever the config says. "Ask before accepting new
  workers" asks in a dialog; no answer within 2 minutes rejects the worker. Regenerate deletes the saved join code
  and restarts hosting, which disconnects every worker (not while rendering).
- **This machine's devices.** The GPU switches are the same as on the Host detail page. "CPU renders too" adds
  `cpu` to the local `--devices`; a CPU that renders keeps "threads kept free" (default 2) for the UI unless
  `--threads` was given. Saved defaults (output folder, film folder) go to the user config file, never the join code.
- **Busy %** counts the work item rendering now, not only finished ones: a long item (a CPU tile at many samples)
  read 0% until it ended, then a capped spike. Stats clamps the 2 s delta to 0-100%.
- **Test hook** `PENUMBRA_TEST_UI_JOIN="<address> <code>"` fills Join a render and presses Join once (D-047
  style, logged). Checked on Windows: the UI joined a `--terminal --host --no-local` controller, rendered, and its
  Leave handed the lease back (requeued).
- **UI Vulkan and the render backends share volk's globals.** The backends' one-time Vulkan probe loads volk's
  global instance functions from its own instance (no surface extension), so after it ran the UI's
  `vkGetPhysicalDeviceSurface*KHR` were null and the first window resize crashed. The UI now runs the probe
  (`gpuInventory()`) before loading its own instance functions, skips frames while the surface is 0 x 0, and
  resets its frame indexes after a swapchain rebuild. Checked: repeated resize, maximize, minimize on Windows.

**D-103 Gate fixes from the first internet run (M10, laptop UI hosting, the box over userspace Tailscale).**
- **The controller sends its own heartbeat** every 5 s (`echo_us` 0) besides echoing the worker's. A worker
  uploading through socat and userspace Tailscale had tens of MB of tiles in those buffers ahead of its heartbeat;
  the echo came back after more than 30 s and the worker dropped a healthy controller (three times in one render,
  each time losing its leases). The controller-to-worker direction carries little, so its heartbeat arrives on
  time. Wire-compatible: workers already ignored heartbeats without an echo. Protocol version unchanged.
- **UI outputs.** A relative output (the default `render.png`) is under the Settings output folder, default
  Pictures/Penumbra (created at Start), not the working folder: started from Explorer that is C:\Windows\system32,
  and the render failed with "cannot open render.penfilm: access denied".
- **Seen, for M9:** a cheap scene (Cornell box, ~230 M samples/s on an RTX 5060 Ti) at 20K over a ~240 ms
  internet link is upload-bound by far; its results arrive late, are reissued, and the GPU mostly waits. Larger
  sample ranges per item for upload-bound workers belong to M9 (network throughput).
- **`--stay` retries a controller that does not answer.** Through a relay (socat, an SSH tunnel) the TCP
  connection always opens, so a controller that is not hosting yet looked like "closed before the controller
  answered", which ended even a `--stay` worker (and the UI's Join). It now waits 1, 2, 4... up to 30 s and tries
  again; a wrong join code still ends it (retrying would get the address blocked).
- **Seen, for M9:** behind the Hyper-V firewall a connection to a port nothing listens on is dropped, not refused,
  so the first attempt of a WSL worker waits for the OS connect timeout (about 2 minutes). A shorter connect
  timeout belongs with the network work in M9.

**D-104 M10 gate passed (2026-10-26).** Host, join and finish a 3-machine render with clicks: the laptop's UI
(Windows 11, i7-11850H, Vulkan UI) hosted; a rented RTX 5060 Ti box (Ubuntu 24.04, its UI on Xvfb + noVNC,
OpenGL) joined over userspace Tailscale through socat; a WSL Ubuntu 22.04 worker joined over the LAN side.
Renders of `scenes/cornell.pen.json` at 4K finished with tiles from all three, e.g. 2048 spp in 69 s at
246 M samples/s (box 254 items, laptop 1, WSL 0 of the CPU-slow tail), no worker dropped after D-103. Also
checked: Viewport live view, Pause/Resume, Save image now, Stop (checkpoint + partial image), Settings, Join a
render and Leave, window resize and fullscreen with Vulkan. The box's own join was started with the
`PENUMBRA_TEST_UI_JOIN` hook; the laptop's Join was clicked. Tests: see the merge commit.

**D-105 Volumetrics: atmosphere and physical sky become milestone M14 (2026-10-27, user decision).** Asked whether
volumetric rendering is possible (for a photoreal mountain range: distant peaks need aerial perspective, which a
renderer without a medium cannot show). Three levels were offered: (1) height fog with closed-form transmittance,
(2) a physical sky from the sun direction, (3) OpenVDB clouds and smoke. The user agreed to levels 1 and 2 as a new
milestone M14 after M8 and M9 (before M11); level 3 stays a later, separate extra (NanoVDB would be a new
dependency, to be asked). Scope and gate are in GUIDE.md "M14". The mountain scene (real elevation data and aerial
photos via `scripts/get-scenes`) is M14's showcase; its region and the converter's language are asked when M14
starts (GUIDE.md "Open decisions").

**D-106 Faster tests (M8 step 0, 2026-10-27, user request: "90% waiting on tests").** The non-GPU suite took 995 s
of test time on the laptop, one test at a time; 11 tests made up ~700 s, mostly waiting on fixed clocks.
- **Reference cache.** Many tests render the same solo reference first (e.g. five fault tests render the identical
  960x540 96 spp Cornell box). `RenderCheck.cmake` keeps references in `build/.../tests/refcache`, keyed by the
  penumbra binary's SHA-256, the scene file, every pixel-affecting argument and the work-item size (chunks merge
  into the film in another order, which changes the last bits); comparisons stay bit-exact.
- **`PENUMBRA_TEST_TIME_SCALE`** (test hook, logged when active, 0.01-1): heartbeats, peer timeouts (worker and
  controller), reconnect backoff and the TLS handshake timeout are multiplied by it. Network tests use 0.2 (peer
  timeout 6 s). Same code paths, shorter clocks; unset it is 1. `net.worker_silent` starts its workers 10 s apart
  instead of 35 s.
- **Side by side.** Every smoke test has its own folder and log (`out.tiff` and `out.webp` shared one
  `out.penfilm`), every render and network test its own config file; the port clash between `device_skipped` and
  `scale_64` (7520) is gone. Test renders use `--threads 4` and declare `PROCESSORS 4` (network tests 8: two
  workers of 4 threads), so `ctest -j <cores>` runs several at once without oversubscribing the cores;
  `scale_64` runs alone (`RUN_SERIAL`). `net.worker_silent` renders 64 spp instead of 256 (it tests the heartbeat
  loss, not the samples).
- **Tiers.** The quick tier (labels unit, smoke and lint; 123 tests, 16 s on the laptop) for everyday edits: `build.ps1 -Quick`,
  `build.sh --quick`, or `ctest -L "^(unit|smoke|lint)$"`. The full suite runs at step commits and the gate, both OSes, as before.
- **`net.scale_64` stays in real time.** With the timers x 0.2 it never finished (ETA over an hour; 105 s at x 1):
  64 workers load the controller, its round trips rise, workers measure them five times as often and deepen their
  queues (about 2 x round trip / item time), queued time counts toward the lease, leases go late and are reissued,
  which loads the controller more. A real feedback loop for an overloaded controller with many workers; for M9
  (scheduler and scale): damp the queue depth and keep queue time out of the late check.
- **CI** also runs on milestone branches (`m[0-9]*`), so Linux and Windows test each pushed step in the cloud
  while work goes on. It skips `scale_64` (label `scale`): on the 4-core runners it ran past its 900 s limit; the
  gate runs it on the laptop.
- **Measured (laptop, i7-11850H, `ctest -j 16`):** full non-GPU suite 445 s wall with references cached, 594 s
  with an empty cache, against 995 s one at a time before; quick tier 16 s. All 170 pass.

**D-107 M8 plan answers (2026-10-27, user).** (1) The gaming PC is out: the user cannot reach it from work. Every
GPU run, including the M8 gate, uses rented boxes; `--vram-limit 8G` stands in for the 8 GB RTX 5060, and since
rented boxes are Linux, GPU results come from Linux with the Windows build and CPU suite as before (as for M5,
D-082). (2) Host acceleration structures: probe and report only (`--diagnose`, placement log); a GPU whose geometry
does not fit in VRAM is tier 3. (3) Intel's New Sponza (with curtains and ivy) joins `scripts/get-scenes` as an
optional download and is M8's real-world scene: its high-resolution textures do not fit in 8 GB, so it walks the
ladder for real. Its license is checked from the download before anything depends on it.

**D-108 Memory budget and fallback (M8 steps 1-6).**
- **The ladder is one pure function** (`core/render/memory_placement`): a device's exact footprint (geometry and
  acceleration structures that must stay in VRAM; the environment map and weights, movable, never downsized; each
  texture, movable and downsizable) against its budgets gives tier 0 (all in VRAM), 1 (textures and environment
  in pinned RAM), 2 (as 1, or in VRAM when it has more room, with the largest texture halved until it fits,
  deterministic) or 3 (the geometry does not fit, the textures fit nowhere with `--no-texture-downsize`, or memory
  ran out at every tier). Unit-tested without a GPU.
- **Exact sizes, live budgets.** Backends measure the real footprint before uploading: Vulkan asks the driver for
  acceleration-structure sizes (every BLAS plus the largest build scratch), CUDA builds its BVH on the CPU first; the
  flattened arrays are kept for the upload. The VRAM budget is read live (Vulkan: `VK_EXT_memory_budget` through
  VMA when the driver has it; CUDA: `cuMemGetInfo`) minus 10%, or `--vram-limit` when smaller. Pinned RAM is one
  pool per host (`--ram-limit`, default 75% of RAM, less the scene the worker holds), shared by its GPUs.
- **Tier 1** puts the texel buffers, the environment map and its weights in host memory the GPU reads over PCIe:
  Vulkan host-visible types that are not device-local (on resizable-BAR GPUs some host-visible types are VRAM),
  CUDA `cuMemHostAlloc` mapped (never managed memory). Shaders do not change; the image is bit-identical to tier 0.
- **Tier 2** halves textures with one shared function (`downsized`, 2x2 means of the stored values); the CPU applies
  it too (its placement or the test hook `PENUMBRA_TEST_TEXTURE_HALVINGS=k`), so GPU tier-2 renders are checked
  against the CPU with the same textures.
- **No out-of-memory crash.** A failed allocation is an `OutOfMemoryError`; the worker gives the tier up, returns its
  RAM to the pool and asks the ladder again one tier lower. Fault mode `oom` injects it (`PENUMBRA_TEST_FAULTS=
  device=N,mode=oom,after=T`: fails at tiers up to T).
- **Uploads are staged in 64 MB parts** through one buffer, so a multi-gigabyte texture buffer needs no staging
  copy as large (upload memory can be device-local on resizable-BAR GPUs).
- **The placement log** is the device summary the controller already logs per device ("tier 1, scene 6.1 GB of
  7.2 GB VRAM, textures 2.4 GB in RAM"); the tier was already on the wire, so the protocol is unchanged. The UI's
  host detail page shows each device's tier and placement while a render runs.
- **Host acceleration structures**: probed (an acceleration-structure buffer allowed in a host-visible, not
  device-local memory type), logged per GPU and shown by `--diagnose`; not used (D-107).

**D-109 Fixes from the first M8 GPU run (RTX 2060 SUPER 8 GB, Vulkan and CUDA).**
- **Texels in parts.** Intel's New Sponza holds 4.9 GB of 8-bit texels; as one storage buffer that is past
  `maxStorageBufferRange` (4 GB - 1 on NVIDIA, and `maxMemoryAllocationSize` is just under 4 GB), so Vulkan read
  wrong texels for the textures past 4 GB (albedo GPU vs CPU relMSE 1.2e-3, yellow surfaces grey; the image still
  looked plausible). Texels now go in up to 4 buffers per kind, each at most 2 GB, a texture never split
  (`gpu::Texture.part`); the shader picks the part with a switch, so no descriptor-indexing feature is needed.
  Limit: 8 GB of 8-bit and 8 GB of float texels per GPU (an `OutOfMemoryError` beyond).
- **A device's first items get a grace period.** Next to the 2060 SUPER's Vulkan RT (an item in 0.14 s), the CPU's
  first items (2 s) passed 10x the median of all devices and were all taken away: the CPU rendered 0 items
  (render.vulkan_hybrid). Until a device finishes one item, its leases are late only after at least 30 s (scaled by
  `PENUMBRA_TEST_TIME_SCALE`); afterwards D-097's rules apply. Hang tests (which hang after 2 items) are unchanged.
- **Measured on the 2060 SUPER:** Intel Sponza (3.7 M triangles, 72 textures, 24 lights) is tier 0 on the real 8 GB
  card, "scene 5.5 GB of 7.3 GB VRAM"; the RTX 2060 SUPER's driver allows acceleration structures in host memory
  (probe).

**D-110 M8 gate passed (2026-10-27), with one known issue.** On a rented RTX 2060 SUPER (8 GB, Turing, driver 580,
CUDA 13.0, Ubuntu 24.04, a 19-core container; the gaming PC is out, D-107):
- **Every tier on a real 8 GB GPU**, Vulkan and CUDA: `tier1_bits` (bit-identical to tier 0), `tier2_vs_cpu`
  (against the CPU with the same halved texture), `tier3_skip`, `no_downsize`, `oom_retry` (injected out-of-memory,
  same bits); GPU suite 42/42 (the 7 multi-GPU tests skip on one GPU).
- **Intel's New Sponza** (3.7 M triangles, 72 textures 4.9 GB, 24 lights), Vulkan 960x540 64 spp: tier 0 "5.5 GB
  of 7.3 GB VRAM" 1.85 M samples/s; tier 1 (`--vram-limit 2G`) "533.3 MB of 2.1 GB VRAM, textures 5.0 GB in RAM"
  0.82 M samples/s (2.3x slower); tier 2 (`--vram-limit 2G --ram-limit 6G`) 67 textures halved, 1.45 M samples/s;
  tier 3 (`--vram-limit 300M`) the GPU skipped with its reason, the CPU finished. No crash at any step. Albedo
  against the CPU: Vulkan relMSE 9.8e-8, CUDA 4.0e-7.
- **Suites:** Windows (laptop) 176/176; Linux (the box) 176/177; CI for step 0 green on both OSes.
- **Known issue, for M9:** `net.scale_64` ran past its 900 s limit on the box (it passes on the laptop in ~105 s):
  with 64 workers on a container throttled to 19 of 80 CPUs, the controller is overloaded, items look 10x slower
  than they are, 18k leases are reissued and the reissues load it further (the loop noted in D-106). It is a
  scheduler problem, not memory; the user chose to merge M8 and fix it first thing in M9 (damp the workers' queue
  depth, keep queue time out of the late check).

**D-111 M9 in progress (2026-10-27; handoff for the next session).** The user approved the M9 plan: (1) the
controller overload loop, (2) `--benchmark`, (3) keep GPUs > 90% busy, (4) CUDA speed (a wavefront kernel only if
profiling shows the megakernel cannot get within ~2x), (5) network: larger sample ranges for upload-bound workers
and a 10 s connect timeout, (6) the gate: measured numbers replace the guide's estimates; LAN workers keep GPUs
> 90% busy. Hardware: the user rents several RTX 5090s and offers a wide range of GPUs for benchmarks; build on the
first box only, then ship a portable bundle (binary + scenes + a run script) to the others; do all laptop work
first and tell the user when a box is worth renting (cost matters).
- **Done (this commit):** a device that keeps delivering results is alive, so its old leases are queued, not lost:
  a lease is late only when its device has also delivered nothing for the late limit
  (`device_last_result_` in `RenderController::reissueLateLocked`). Fault and hang tests pass (15/15).
- **Found:** that rule does not fix `net.scale_64` under load (with `PENUMBRA_TEST_TIME_SCALE=0.2` on the laptop it
  still reissued 15k leases). Root cause: the controller decompresses and merges every `TileResult` on one thread;
  512 null devices send about 1,700 tiles/s, results queue up in the controller, devices look silent and their
  leases are reissued. The same limit will hold back fast GPUs (5090s) on a LAN.
- **Next, in order:** multi-threaded result decode and merge in the controller, backpressure so workers slow down
  when it falls behind, and a measured controller tiles/s; then `--benchmark` (`controller/benchmark.{h,cpp}`:
  each device of the host alone, repo scenes plus `scenes/sponza` / `scenes/intel-sponza` when downloaded,
  time-limited 1080p, device samples/s, wall samples/s, busy %, Markdown for `docs/BENCHMARKS.md` plus JSON); then
  the network items; then the GPU session (profiling, busy %, CUDA tuning) on a rented 5090 box.

**D-112 Controller throughput (M9 step 1).** Measured, not guessed: every render now logs results/s and how long
the controller spent unpacking tiles, merging them, handing out work (and of that the tail search), the late
check, and how busy the network threads were (and of that TLS decryption). On `net.scale_64` (64 workers, 512 null
devices) the first measurement disproved the overnight guess (D-111): unpacking and merging were 11% of a core, but
the single network thread was 96-98% busy and the controller process sat at exactly one core.
- **Tail search**: once the queue is empty, every waiting device scanned all outstanding leases twice (is it idle?
  which lease to copy?) on every result: 512 devices x ~1,500 leases, 48.5 of 93 busy seconds. Now one scan per
  hand-out call records the busy devices, the copy search stops at the first eligible lease (lease ids grow with
  time, so it is the oldest), and a device at least as slow as one that found nothing skips the search. Same
  choices as before. 95 s -> 48 s.
- **Several network threads**: what remained was TLS decryption (21 of 47 s) and unpacking, all on one thread. The
  controller's `IoThread` now runs 2-8 threads (one per 4 usable cores) and every connection has its own asio strand,
  so a connection's steps stay in order while connections are handled in parallel. The worker pointer the watchdog
  reads is atomic, and an approval answer runs on its connection's strand. Workers keep one network thread.
  48 s -> 19.4 s; 104 -> 442 results/s; 300 -> 1,748 M samples/s.
- **A device that keeps delivering keeps its leases** (D-111's rule) stays: a lease is late only when its device has
  also been silent for the limit.

**D-113 Worker heartbeats while preparing; a quicker first join (M9, the 3-machine datacenter test).** First run
across three rented boxes in one datacenter (controller RTX 4060 Ti, workers RTX 5060 Ti and RTX 4060 Ti):
Khronos Sponza at 4K 1,024 spp rendered on all three at 127 M samples/s, 90% of the three GPUs' solo speeds added,
every GPU 94-100% busy while it had the scene. Intel's New Sponza showed two worker bugs:
- **No heartbeats while preparing.** A worker loads a big scene (35 s from its cache) and builds it on its devices
  (8 s) on the thread that sends heartbeats; after 30 s of silence the controller dropped it, it reconnected, loaded
  again and never joined. A keepalive thread now sends heartbeats while the worker prepares.
- **A `--stay` worker that has never joined** backed off 1, 2, 4, 8 s while the controller was not up yet and
  joined 12 s late; until its first Welcome the wait is at most 2 s (30 s still after a lost controller).
- The 2.2 GB scene package reached two workers at once at ~117 MB/s in total: the controller was on 1 Gbit/s
  Ethernet (user), so the transfer ran at line rate. Not a Penumbra limit; recheck only on a 10 GbE controller.

**D-114 `--benchmark`, and what the step-3 "GPU idle" really was (M9 steps 2-3).** Reading the Vulkan render
loop: a dispatch is at most 4M paths, so on Intel Sponza (~12 M samples/s) each CPU round trip (~0.1-0.2 ms) sits
between ~0.35 s of GPU work (<0.1% idle), and on Cornell (~15 ms dispatches) about 1%, which matches the measured
97-99%. Between items a slot copies back one small tile and takes the next of its two queued leases. Neither
explains 78%. What does: `RenderController::run` starts the render clock, and with it `--time-limit`, before the
local worker uploads and builds the scene, and the solo script averaged every non-zero `nvidia-smi` sample,
upload included. So no change to the dispatch loop without a measurement that shows a gap.
- `--time-limit` keeps counting from the start (a user's time budget includes the upload).
- Every `DeviceTally` now records its render window: from its first item's start (result time minus render time)
  to its last result, plus backend, upload + build time and placement.
- `penumbra --benchmark` renders each scene (the one given, else `cornell`, `spheres`, `sponza`, `intel-sponza`
  from `./scenes` when present) on each selected device alone, through the normal controller -> scheduler -> local
  worker -> film path. Defaults are 1080p and 30 s, unless `--res`, `--time-limit` or `--spp` is given. The scene is
  loaded once per scene, with a new film per device. It reports M samples/s while rendering (device speed),
  M samples/s over the window, busy % (rendering / window) and upload + build seconds. It writes JSON (`-o`, default
  `penumbra-benchmark.json`) and a `.md` table. Comparing backends: `--devices 0:vulkan,0:cuda`.
- Known, unchanged: a work item is 2^26 pixel-samples, so a CPU alone (~2 M samples/s on the laptop) needs ~33 s per
  item, and `--time-limit` waits for the item in progress plus the queued one (a 10 s limit ended after 67 s).
  On GPUs an item takes well under a second.

**D-115 CUDA speed (M9 step 4): measured, one experiment withdrawn, the traversal is the bottleneck.** On three
Blackwell boxes (RTX 5060 Ti, 5070 Ti, RTX PRO 6000; BENCHMARKS 2026-10-29) CUDA is 4-19x slower than Vulkan RT at
100% busy. The containers refuse GPU performance counters (`ncu`: ERR_NVGPUCTRPERM), so the cause was narrowed down
with experiments:
- ptxas (`-Xptxas -v` is now in every CUDA build log; `PEN_CUDA_NVCC_FLAGS` passes extra nvcc flags): the `render`
  kernel uses 168-169 registers and a 1576-byte stack frame on sm_120 (compute_75 PTX), no spills.
- Each architecture from its own PTX (commit 413c890) cut the sm_120 frame to 848-936 bytes but was slower on Sponza
  on all three boxes (5.5 vs 7.0, 12.6 vs 15.3, 19.0 vs 26.1 M samples/s). Reverted: one compute_75 PTX stays (D-087).
- Register caps (`-maxrregcount` 128/96/64, 72-884 bytes of spills): -20% to +30% on `path`, no change on
  `debug:normal` (186-197 M samples/s at 64, 96 and 168 registers). Occupancy is not what limits traversal. Not
  adopted: no cap wins on every GPU.
- Traversal alone (Sponza, RTX 5070 Ti): `debug:normal` 1,399 (Vulkan) vs 200 (CUDA) M samples/s, `ao` 748 vs 86.
  A primary ray takes ~40 traversal steps (`debug:bvh`, after Sponza's +1.5 exposure), a normal CWBVH count, so the
  tree is fine and each step is slow.
- Next (not today: no counters, and every try costs box time): rework the CWBVH loop after tinybvh's CUDA
  kernel (traversal stack in shared memory, node loads, a traversal-only kernel if needed). Until then the measured
  numbers above replace the guide's 2-5x estimate in the README.
- Open: in the first three-box run, one worker's 58 MB scene transfer made no progress for 69 s while the other
  finished in 0.4 s; raw TCP on the same route ran at 62 MB/s, and two reruns transferred in 1.0 s. Not reproduced;
  a transfer that makes no progress for a while should be logged (and maybe restarted).
- Gate check (busy GPUs): every device 99-100% busy alone (`--benchmark`), and internet workers ~98% busy from
  their first item to their last result in a three-datacenter render. `--stay` workers rejoined a new controller
  16 s late (backoff up to 30 s after a lost controller, D-113); a shorter retry would help back-to-back renders.

**D-116 Network work (M9 step 5): items of several chunks for upload-bound workers, 10 s connect timeout,
quicker `--stay` rejoin, transfer-stall warning.** User decision (2026-10-29): bigger items only for workers that
fall behind on upload.
- **Why:** a result carries the whole tile whatever its sample count (~0.5-1 MB for 256 x 256), so one fast GPU at
  ~300 M samples/s sends ~20-25 Mbit/s and a 4-GPU server over 100 Mbit/s: more than many home uplinks (D-103 saw
  a Cornell box at 20K over a 240 ms link upload-bound by far).
- **How:** `Stats` carries `upload_queued` (protocol 5). The controller divides it by the worker's upload rate;
  when three reports in a row (6 s) show more than 2 s and 4 MB waiting, the worker's span doubles (2, 4, 8 chunks per item, at most every 4 s), and after 20 s
  below 0.5 s it halves. A span item is consecutive chunks of one tile (`next(span)`): its id is the first
  chunk's, its sample range covers all of them, so validation, requeue, tail copies and in-order merging work as
  before, and `markMergedUntil` advances the tile by the whole span. The film still holds whole chunks, so
  `--resume` is unchanged. Item times are kept per chunk, so the late check judges a span item by its age / span.
  Breadth-first passes stay even: a tile given 2 chunks sits out the next pass. Every change is logged at info.
- **Determinism:** renders where no worker falls behind keep one chunk per item and stay bit-identical for any
  distribution (D-045). A merged span differs from chunk-by-chunk merging only in rounding (`net.item_span`:
  relMSE 7.8e-16 against the solo render, max 3.8e-6 per value, with items of 3-4 chunks; test hook `PENUMBRA_TEST_ITEM_SPAN` forces a span, logged when active).
- **Connect timeout:** `connectTo` connects asynchronously and gives up after 10 s (`kConnectTimeout`): behind a
  firewall that drops instead of refusing (Hyper-V in front of WSL, D-103) the first attempt waited ~2 minutes.
- **`--stay` rejoin:** after a lost controller a worker now retries at least every 5 s (was 30 s): in the
  three-datacenter test, workers missed the first 16 s of a back-to-back render (D-115). The 30 s backoff stays for
  a relay that answers while no controller is behind it.
- **Transfer stall:** a scene transfer that hands no chunk to its connection for 15 s is logged with its chunk and
  queued bytes, and again when it moves (D-115's unexplained 69 s stall).

**D-117 M9 gate passed (2026-10-29, approved by the user).** Measured speeds replace the estimates: `penumbra
--benchmark` on an RTX 5060 Ti, RTX 5070 Ti and RTX PRO 6000 (BENCHMARKS 2026-10-29); README and guide give CUDA as
4-19x slower than Vulkan RT (was estimated 2-5x). Busy GPUs: every device 99-100% busy alone, and workers ~98% busy
from their first item to their last result in a three-machine render. The workers were in three datacenters over
the internet, not a LAN (the harder case; accepted). Controller: `scale_64` 95 s -> 19 s; in the cluster render
under 1% of a core merging and 2% on the network. Windows: full suite 181/181 at cda991b. Linux: CI passed at
2e8e165 and three Ubuntu 24.04 boxes built from scratch by the README steps; CI for cda991b (step 5) was still
running at the merge (the user chose not to wait; a failure gets fixed on main). Not done in M9: the CUDA traversal
rework (D-115), a separate job that needs a GPU box. Protocol version 5. Next: M14 (plan posted, answers: Swiss
Alps, C++ converter, Blender headless).

**D-118 M14 starts (2026-10-29): plan approved, open decisions answered.** Plan: (1) `.pen.json` `atmosphere` and
`sky` in the settings schema (flag, config, UI, README), off by default; (2) the CPU reference first: exponential
height fog with closed-form optical depth and analytic free-flight sampling, a Henyey-Greenstein phase function,
next-event estimation to lights, sun and environment through the fog, MIS; (3) a physical sky (Nishita single
scattering: Rayleigh, Mie, ozone) baked at load into a latitude-longitude environment map, so every backend reuses
the HDRI sampling, plus a sun disk light from the same direction; (4) the Slang port for Vulkan and CUDA;
(5) UI and README; (6) the mountain scene and an 8K render on a GPU box. User answers: the mountain scene is the
**Swiss Alps** (swissALTI3D elevation and SWISSIMAGE photos from swisstopo; open data, credit "(c) swisstopo"; 2 m
photos for the wide area, sharper tiles only near the camera to keep the download to a few GB); the terrain
converter is a **C++ tool in `tools/`** (libtiff, libjpeg, libpng are already dependencies; swisstopo tiles are named
by their LV95 coordinates, so no GeoTIFF library); the physical sky is compared with **Blender's Nishita sky**,
rendered headless from a portable Blender on D: (not a system install, not a dependency of Penumbra).

**D-119 The atmosphere's design (M14 steps 1-2, CPU).** `.pen.json` `"atmosphere"` and `"sky"` are scene
description objects like `"environment"` and `"camera"` (no CLI flags); serialized for sessions and workers, and
part of the pixel hash only when enabled, so renders without them keep their hash (and resume).
- **Medium:** up to three exponential height layers, density exp(-(y - base) / height): Earth's air (Rayleigh
  phase, 5.8/13.6/33.1e-6 per m, 8 km) and haze (Mie as in Blender's Nishita: 2e-5 per m scattering, 11%
  absorption, 1.2 km, Henyey-Greenstein g 0.76), plus a user ground fog (density per m, scale height, base, albedo,
  g). `altitude` puts y = 0 above sea level; `meters_per_unit` converts. Air and haze follow the sky's `air` and
  `dust` when a sky is set, so haze and sky match. Ozone only in the sky (negligible across a scene).
- **Sampling:** optical depth along a ray has a closed form (expm1/log1p for accuracy), so transmittance is exact
  and free-flight distances invert it; no ray marching, no majorant. RGB coefficients: one colour channel is
  picked per segment, each layer's collision distance sampled for it, the earliest wins (independent layers);
  weights divide by the average over the channels (spectral MIS). At a collision: next-event estimation to every
  light (area, punctual, environment) through the medium's transmittance, MIS with the phase function, then the
  phase function picks the direction (Rayleigh by Cardano, HG by inversion). Surface NEE multiplies by the same
  transmittance. A scattering counts as a bounce (max_bounces, Russian roulette).
- **Determinism:** the medium draws from its own sampler dimensions (65536 + 8 per bounce), so renders without an
  atmosphere keep their exact numbers; the surface code multiplies by the transmittance only when there is a
  medium. Golden images unchanged (full suite).
- **Unbounded fog:** below its base a layer keeps getting denser (capped at e^80); without ground, a ray going
  down does thousands of scatterings (an empty-scene furnace lost 12% to the bounce cap and fireflied). Real
  scenes have terrain; the furnace test has a white ground 20 km wide.
- **Tests:** depth vs numeric integration and its inverse; uniform-medium transmittance equals exp(-sigma t)
  (gate); free-flight sampling unbiased per channel (pass branch = T, scatter branch = 1 - T); phase sampling
  matches the value, mean cosine g (HG) and E[cos^2] = 2/5 (Rayleigh); layers from the settings; render furnace
  (non-absorbing fog over a white Lambert ground, white environment): 16x16 at 65,536 spp, every pixel within
  0.78% of 1, mean 1.00006.
- **Until the GPU port (step 4)** Vulkan and CUDA refuse a scene with an atmosphere (no silent fallback).

**D-120 The physical sky (M14 step 3, CPU bake).** `"sky"` replaces the environment with a map baked at load
(`core/scene/sky.cpp`), then sampled like an HDRI on every backend (no new GPU code). Written from the physics and
published data, not from Blender's code (Blender is GPL; Penumbra is MIT): Nishita single scattering over a
spherical Earth (radius 6360 km, atmosphere 60 km), 21 wavelengths 380-780 nm; Rayleigh from the refractive index of
air (Peck and Reeder) with the King factor; Mie as Blender's (2e-5 per m, 1.11 extinction, 1.2 km, Cornette-Shanks
g 0.76); ozone as the Chappuis band (Gaussian, peak 4.6e-25 m^2 at 603 nm, 5.4e18 per m^3 at 25 km, 10-40 km
tent); the sun a 5778 K black body (1361 W/m^2); CIE 1931 via the Wyman-Sloan-Shirley fit; XYZ to linear Rec. 709.
The sky depends only on view elevation and azimuth from the sun: a 512 x 512 table (elevation spaced x|x|, dense at
the horizon) with bilinear lookup; view rays 64 steps and sun rays 32, spaced (k/n)^2 near their start. About 1.5 s
per load on the laptop. The sun disc: 8 x 8 subsamples per pixel (power independent of the map resolution, tested
at 2048 and 4096 within 2%), limb darkening 1 - 0.6 (1 - cos) normalized to the full irradiance.
- **Against Blender 4.2.23** (headless from a portable copy on D:, Cycles, Nishita, sun 30 deg, altitude 1 m):
  above 10 deg elevation within 0-5% in every channel, towards and away from the sun, with no scale constant (both
  in W/m^2/sr). Near the horizon Blender is up to 33% darker and redder. Cause found: Blender adds each 1/32 step's
  whole optical depth before using it, so every sample sees half a step (~9 km at the horizon) too much air.
  Replicating that in Penumbra matched Blender within 0-6% everywhere, so the physics and data agree. Kept the
  accurate integration: the in-scene haze (D-119) is exact, so an accurate sky meets the hazy terrain without a seam.
  Blender's sun disc has 18% less power (its limb darkening is not normalized); Penumbra keeps the real irradiance.
  Unit tests pin Blender's zenith, 20 deg away from the sun and 50 deg towards it (within 8%).
- Physical units: the sky needs an exposure around -6 to -10, as in Blender. `sun_strength` scales the disc only,
  like Blender's Sun Intensity. A sky together with an HDRI: the sky wins (warning).

**D-121 The atmosphere on the GPUs (M14 step 4).** `shaders/medium.slang` is the twin of `core/render/medium.h`
(Slang has no expm1, log1p or cbrt: a two-term series below 1e-4, and pow(x, 1/3) where x is always positive), and
`integrator.slang` follows the CPU integrator: the medium's own sampler dimensions, NEE through the medium at
scattering points, the surface shadow rays multiplied by the transmittance only when there is a medium (so images
without one keep their bits). The layers travel in `FrameData` (`medium_count` + 3 x 40-byte layers; Frame 172 ->
296 bytes). CUDA layout checked on the laptop with slangc's reflection: `medium_count` at 172, `medium` at 176,
FrameData 296 bytes, as gpu::Frame; slangc does not report the nested MediumLayer on its own, so it is covered by
the FrameData size. Vulkan and CUDA no longer refuse an atmosphere. Tests: `slots_atmosphere` (CPU, 1, 2 and 3
slots give the same bits with sky, air, haze and fog); GPU (run on a box at the gate): `atmosphere_vs_cpu` (relMSE
< 1e-3 at 4,096 spp) and `furnace_fog` on Vulkan and CUDA.

**D-122 The Swiss Alps scene and penumbra-terrain (M14 steps 5-6).**
- **UI (step 5):** the Render page has a "Sky and atmosphere" section (open when the scene uses them): physical
  sky (sun elevation, azimuth, disc strength, air, dust, ozone, altitude) and atmosphere (haze, fog density and
  height with logarithmic sliders, fog base, scene altitude). The framing preview re-applies them to a copy of the
  loaded scene (`applyEnvironment`, the environment/sky/medium part of loadScene), since the sky is baked at load.
  The exposure slider reaches -12 (a physical sky needs about -8).
- **Region:** Eiger, Mönch, Jungfrau (Bernese Oberland) instead of the Matterhorn: entirely in Switzerland, while
  swisstopo's data stops at the Italian border on the Matterhorn's south side. 16 x 16 km, LV95 E 2632-2648 km,
  N 1150-1166 km; camera 2700 m over the Männlichen ridge (668 m above the ground) towards the Mönch, 40 degree
  vertical field of view; evening sun (22 degrees, azimuth 300) on the north faces; light valley fog at 1100 m. The
  data checks: highest point 4158.2 m (the Jungfrau) and 4107.1 m in the Mönch's tile, both at their summits.
- **Data:** swissALTI3D 2 m (float32 GeoTIFF, LZW, 500 x 500 per km) and SWISSIMAGE 2 m (RGB, JPEG in YCbCr), the
  newest year of each tile (tiles are listed once per survey year). The STAC API lists every asset with a SHA-256
  (`checksum:multihash`, prefix 1220): get-scenes verifies every tile. 512 tiles, 323 MB, 27 s on the laptop.
- **penumbra-terrain** (`tools/terrain.cpp`, C++ as decided): `--stac-next` / `--stac-list` read the STAC JSON so
  the scripts need no JSON parser (Ubuntu has no jq; mawk lacks gawk's match arrays); the conversion mosaics the
  tiles, writes one grid mesh (a vertex every --grid m, smooth normals from central differences), the photo
  texture (--texture m per pixel, JPEG via stb) and a glTF with a camera placed from LV95 coordinates. Axes: x east,
  y metres above sea level, -z north, so `atmosphere.altitude` is 0 and a sky azimuth is a compass bearing.
  The full scene: 4000 x 4000 vertices, 32 M triangles, 0.9 GB, an 8000 x 8000 texture; 30 s to build.
- **libtiff with JPEG:** vcpkg's tiff was built with zip only; the photo tiles need its jpeg feature (libjpeg-turbo
  is already a dependency, so no new library; tiff is rebuilt once, about a minute).

**D-123 M14 on the GPU and the 8K mountains (rented RTX 5090, 32 GB, driver 595.71, Ubuntu 24.04, CUDA 13).**
- **GPU suite** (`ctest -L gpu`, m14 53bb80c): 45 of 46 passed at first, including `vulkan_atmosphere_vs_cpu` and
  `cuda_atmosphere_vs_cpu` (sky, air, haze and fog against the CPU, relMSE < 1e-3 at 4,096 spp), `cuda_furnace_fog`,
  and every earlier golden, furnace and cross-backend test on both backends (so images without an atmosphere are
  unchanged). `vulkan_furnace_fog` gave mean 0.898. Not the fog: with the fog density at 1e-9 Vulkan still gave
  0.937 against CUDA's 1.000. The furnace ground was two triangles 20 km across; with vertices 10 km out, the
  hardware's hit points near the camera carried ~1 mm of float error and shadow rays caught their own triangle.
  The ground is now 100 x 100 quads of 200 m (`tools/scenegen.cpp`): Vulkan, CUDA and CPU fog furnaces pass, so 46
  of 46. Only giant triangles are affected (the terrain's are 4 m); not changed in the renderer.
- **The Swiss Alps at 8K** (7680 x 4320, 1024 spp, Vulkan): `get-scenes.sh swiss-alps` from scratch on the box
  (512 tiles checked, scene built) in 59 s; the render 78.4 s, 34.0 G samples, 433 M samples/s, 3.6 GB of VRAM
  (tier 0), GPU ~86% busy by nvidia-smi over the whole run including the load. Eiger, Mönch and Jungfrau with their
  glaciers, the evening sun on the north faces, the physical sky.
- **Known limit of the data:** on near-vertical faces (the Eiger's north face) the orthophoto, taken from above, is
  stretched into vertical streaks. Inherent to draping an aerial photo; fixing it (e.g. triplanar texturing of the
  steep parts) would be a renderer feature, not part of M14.
- The M9 CI run for cda991b finished later: Linux and Windows passed (D-117 had left Linux unconfirmed).

**D-124 M14 gate passed (2026-10-29, approved by the user).** Transmittance equals the closed form (unit test);
the fog furnace is uniform on CPU, Vulkan and CUDA; CPU, Vulkan and CUDA match with sky, air, haze and fog (relMSE
< 1e-3 at 4,096 spp, RTX 5090); 1, 2 and 3 slots give the same bits; with no atmosphere every golden and comparison
is unchanged on all three backends; the sky is within 5% of Blender's Nishita sky above 10 degrees (the horizon
difference is Blender's integration bias, D-120); the Swiss Alps render at 8K on the RTX 5090 in 78 s (D-123).
Windows full suite 192/192; Linux CI passed through e16d266, the last four commits' runs were still going at the
merge (the converter and libtiff's JPEG were built and run on Ubuntu 24.04 on the box). Next: M11 (denoise,
tonemapping, AOVs), then M12; M13 optional; the CUDA traversal rework (D-115) separate.

**D-125 M11 starts (2026-10-30): plan approved; OIDN from Intel's release binaries.** Plan: (1) tonemapping in the
output stage: AgX as Blender 4.x (published matrices and curve), Blender's Filmic, the ACES fit; checked against
Blender's view transforms on the same EXR (portable Blender on D:); (2) AOVs albedo, normal, depth at the first
non-specular hit on CPU, Vulkan and CUDA, stored as film channels merged like the image, sent with tiles (protocol
6), written as EXR layers or sibling files; (3) OIDN 2 per output tile with a 32 px border, using albedo and normal,
on the controller's CPU or its NVIDIA GPU (OIDN's CUDA device); on for UI previews, off for --terminal (guide
default); (4) metadata in EXR headers and PNG text; (5) the UI's Denoise and Tonemap controls, live; (6) gate: a
40K denoised render without seams (differences across tile edges against inside tiles) on a GPU box, and AgX close
to Blender. User answer: vcpkg has no OIDN port, so the build scripts download Intel's prebuilt OIDN 2.x release
for Windows and Linux (CPU and CUDA devices), pinned to a version and SHA-256, and ship its libraries next to
penumbra (the guide lists OIDN, Apache-2.0).

**D-126 Tonemapping (M11 step 1).** `core/output/tonemap.cpp`, applied after the exposure in the output stage, the
UI preview and the live viewport (the viewport's tonemap is live like its exposure). Reference: 217 linear colours
(grey from -10 to +8 EV, 12 hues x 3 saturations x 5 EVs) through Blender 4.2.23's view transforms (sRGB display,
look None), headless from the portable copy; the table is `tests/data/tonemap_blender.json` (Blender's output
numbers), checked by a unit test.
- **AgX:** the published Rec. 2020 inset and outset matrices (Filament, three.js) with the log2 range -12.47 to
  +4.03 EV. The widely copied 6th-order polynomial for the sigmoid is not Blender 4.2's curve (18% grey 0.50 instead
  of 0.46; mean 6.8, max 52 8-bit steps off), so the curve is Blender's measured one (the value before the outset
  that reproduces its grey, every half EV). Result: grey within a step, mean 1.4 steps over all colours, max 35 on
  fully saturated primaries. Fitting the outset matrix to Blender's output did not help (mean 1.5, max 27), so
  the remaining difference is in Blender's inset/outset construction; accepted as "close to Blender" (gate).
- **Filmic:** Blender's Filmic (Medium Contrast) grey response, measured the same way, per channel: grey exact,
  colours mean 2.5 steps (Blender also desaturates bright colours with a 3D LUT, not reproduced).
- **ACES:** Stephen Hill's fit of the RRT and sRGB ODT (BakingLab, MIT). **None:** the sRGB curve with clipping,
  equal to Blender's Standard (max 0.004 steps).
- No code or LUT from Blender: its output numbers for our inputs are measurements.
