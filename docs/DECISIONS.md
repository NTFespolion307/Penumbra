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
