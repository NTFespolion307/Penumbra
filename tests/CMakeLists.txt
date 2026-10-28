find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

set(PEN_TEST_DATA "${CMAKE_CURRENT_SOURCE_DIR}/data")

# ---- Unit tests ----
add_executable(penumbra_unit_tests
  unit/test_bsdf.cpp
  unit/test_cli.cpp
  unit/test_config.cpp
  unit/test_film.cpp
  unit/test_memory.cpp
  unit/test_net.cpp
  unit/test_package.cpp
  unit/test_parse.cpp
  unit/test_presets.cpp
  unit/test_render_path.cpp
  unit/test_sampling.cpp
  unit/test_scheduler.cpp
  unit/test_settings.cpp)
target_link_libraries(penumbra_unit_tests PRIVATE penumbra_cli penumbra_controller penumbra_net Catch2::Catch2WithMain)
target_compile_definitions(penumbra_unit_tests PRIVATE PEN_TEST_DATA_DIR="${PEN_TEST_DATA}")
# Unit tests count chunks of the M3 size (2^22 pixel-samples, DECISIONS D-083) unless they pass a budget.
catch_discover_tests(penumbra_unit_tests PROPERTIES LABELS unit ENVIRONMENT "PENUMBRA_TEST_CHUNK_BUDGET=4194304")

# ---- Smoke tests: run the real binary and check exit codes and messages ----
# Each runs in its own folder with its own log: they run side by side (ctest -j), and outputs such as out.tiff and
# out.webp would otherwise share one out.penfilm (D-106).
set(PEN_SMOKE_COMMON "--config|${PEN_TEST_DATA}/empty-config.json|--log-file|SMOKE_LOG")
function(pen_smoke name expect args)
  set(dir "${CMAKE_CURRENT_BINARY_DIR}/smoke/${name}")
  file(MAKE_DIRECTORY "${dir}")
  string(REPLACE "SMOKE_LOG" "${dir}/smoke.log" args "${args}")
  set(cmd "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:penumbra> "-DARGS=${args}" -DEXPECT=${expect} -DWORKDIR=${dir})
  if(ARGC GREATER 3)
    list(APPEND cmd "-DMATCH=${ARGV3}")
  endif()
  add_test(NAME smoke.${name} COMMAND ${cmd} -P "${PROJECT_SOURCE_DIR}/cmake/ExpectExit.cmake")
  set_tests_properties(smoke.${name} PROPERTIES LABELS smoke)
endfunction()

pen_smoke(version 0 "--version" "Penumbra [0-9]+\\.[0-9]+\\.[0-9]+ \\(git ")
pen_smoke(help 0 "--help" "--max-bounces")
pen_smoke(merge_help 0 "merge|--help" "Partial films")
pen_smoke(list_devices 0 "--list-devices" "cpu")
pen_smoke(diagnose 0 "--diagnose" "Scene cache")
pen_smoke(unknown_flag 1 "--no-such-flag")
pen_smoke(spp_out_of_range 1 "${PEN_SMOKE_COMMON}|--terminal|missing.glb|--spp|5000000000" "out of range")
pen_smoke(webp_size_limit 1 "${PEN_SMOKE_COMMON}|--terminal|missing.glb|--res|20k|-o|out.webp" "16383 px per side")
pen_smoke(missing_scene 2 "${PEN_SMOKE_COMMON}|--terminal|missing.glb" "scene file not found")
pen_smoke(worker_needs_join 1 "${PEN_SMOKE_COMMON}|--worker|--code|K7QD-M2XR-9FHT-4WNB" "--join")
pen_smoke(host_needs_terminal 1 "${PEN_SMOKE_COMMON}|--host" "--terminal")
pen_smoke(no_local_needs_host 1 "${PEN_SMOKE_COMMON}|--terminal|missing.glb|--no-local" "needs --host")
pen_smoke(worker_bad_address 1 "${PEN_SMOKE_COMMON}|--worker|--join|host:99999|--code|K7QD-M2XR-9FHT-4WNB" "port must be")
pen_smoke(worker_nothing_listening 7 "${PEN_SMOKE_COMMON}|--worker|--join|127.0.0.1:1|--code|K7QD-M2XR-9FHT-4WNB"
          "cannot connect")
pen_smoke(empty_scene_renders 0 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/empty.gltf|--res|240p|--spp|1|-o|empty.png"
          "426x240.*no lights")
pen_smoke(benchmark 0 "${PEN_SMOKE_COMMON}|--benchmark|${PROJECT_SOURCE_DIR}/scenes/cornell.pen.json|--devices|cpu|--threads|2|--res|120p|--spp|4|-o|bench.json"
          "\\| cpu \\| cornell \\| [0-9.]+ \\| [0-9.]+ \\| [0-9]+ \\|")
pen_smoke(pen_json 0 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/scene.pen.json|--scale|0.05|--spp|1|-o|pen.png"
          "128x72 \\(2k at scale 0.05\\).*camera 'Camera.001'")
pen_smoke(exr_half 0 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/scene.pen.json|--res|64x36|--spp|1|-o|half.exr" "wrote")
pen_smoke(exr_float 0 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/scene.pen.json|--res|64x36|--spp|1|--bit-depth|32|-o|float.exr" "wrote")
pen_smoke(png_16bit 0 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/scene.pen.json|--res|64x36|--spp|1|--bit-depth|16|-o|deep.png" "wrote")
pen_smoke(jpeg 0 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/scene.pen.json|--res|64x36|--spp|1|-o|out.jpg" "wrote")
pen_smoke(tiff 0 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/scene.pen.json|--res|64x36|--spp|1|-o|out.tiff" "wrote")
pen_smoke(webp 0 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/scene.pen.json|--res|64x36|--spp|1|-o|out.webp" "wrote")
pen_smoke(resume_missing 1 "${PEN_SMOKE_COMMON}|--terminal|--resume|missing.session.json" "cannot open session")
pen_smoke(merge_missing 1 "${PEN_SMOKE_COMMON}|merge|missing.penfilm|-o|m.exr" "cannot open")
pen_smoke(optix_not_yet 3 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/empty.gltf|--backend|optix|-o|x.png" "M13")
pen_smoke(debug_bvh_needs_cuda 1 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/empty.gltf|--integrator|debug:bvh|-o|x.png"
          "add --backend cuda")
pen_smoke(unknown_camera 2 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/empty.gltf|--camera|Nope|-o|x.png" "not in the scene")

# ---- Gate tests (M1): scenes are generated at build time by penumbra-scenegen ----
set(PEN_SCENES "${CMAKE_CURRENT_BINARY_DIR}/scenes")
add_custom_command(OUTPUT "${PEN_SCENES}/cornell.gltf"
  COMMAND penumbra-scenegen "${PEN_SCENES}"
  DEPENDS penumbra-scenegen
  COMMENT "Generating test scenes")
add_custom_target(penumbra_test_scenes ALL DEPENDS "${PEN_SCENES}/cornell.gltf")

function(pen_render_test name mode scene args)
  add_test(NAME render.${name}
           COMMAND "${CMAKE_COMMAND}" -DPENUMBRA=$<TARGET_FILE:penumbra> -DDIFF=$<TARGET_FILE:penumbra-diff>
                   -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR} -DNAME=${name} -DSCENE=${PEN_SCENES}/${scene} "-DARGS=${args}"
                   -DMODE=${mode} ${ARGN} -P "${PROJECT_SOURCE_DIR}/cmake/RenderCheck.cmake")
  # Renders use every core; ctest -j counts this many, so several still run side by side without starving the
  # timing-sensitive ones (D-106).
  set_tests_properties(render.${name} PROPERTIES LABELS "render;gate" PROCESSORS 4)
endfunction()

# Furnace: white Lambert sphere and smooth glass sphere in a uniform white environment; every pixel within 0.5%.
# The diffuse sphere is converged at 16,384 spp (MIS noise falls below 0.5% per pixel); the smooth glass sphere
# has no variance beyond reflect/refract choices, which all return 1.
pen_render_test(furnace_diffuse uniform furnace_diffuse.pen.json "--res|32x32|--spp|16384" -DTOLERANCE=0.005)
pen_render_test(furnace_glass uniform furnace_glass.pen.json "--spp|256" -DTOLERANCE=0.005)
# The same image for any thread count, bit for bit.
pen_render_test(threads_cornell threads cornell.pen.json "--res|64x64|--spp|16" "-DTHREADS=1|2|0")
pen_render_test(threads_spheres threads spheres.pen.json "--res|96x54|--spp|8" "-DTHREADS=1|3|0")
# Cornell box against the approved golden image (DECISIONS D-021).
pen_render_test(golden_cornell reference cornell.pen.json "--res|256x256|--spp|256"
                -DREFERENCE=${PROJECT_SOURCE_DIR}/tests/golden/cornell_256_4096spp.exr -DMAXRELMSE=1e-3)
# 256 spp against the 4,096 spp golden measures relMSE 3.4e-4 (noise); a 5% brightness shift exceeds 1e-3.
# Every material, the HDRI, AO and the debug views render.
pen_render_test(spheres runs spheres.pen.json "--res|160x90|--spp|16")
pen_render_test(ao runs spheres.pen.json "--res|160x90|--spp|4|--integrator|ao")
pen_render_test(debug_normal runs spheres.pen.json "--res|160x90|--spp|1|--integrator|debug:normal")
# M2: kill + --resume gives the same image bit for bit; merging region and sample-range halves reproduces the
# single render; a --region render equals that crop of the full render; every preset renders.
pen_render_test(resume resume cornell.pen.json "--res|1080p|--spp|16" -DKILL_AFTER_ITEMS=4 -DSMALL_CHUNKS=1)
pen_render_test(merge_regions merge_regions cornell.pen.json "--res|1080p|--spp|4")
pen_render_test(merge_samples merge_samples cornell.pen.json "--res|1080p|--spp|8")
pen_render_test(region_crop region_crop cornell.pen.json "--res|1080p|--spp|4")
pen_render_test(presets presets cornell.pen.json "--spp|1|--scale|0.01")
set_tests_properties(render.resume PROPERTIES LABELS "render;gate;slow")
# M3: N local slots match 1 slot bit for bit; failed and hung devices are recovered without changing the image.
pen_render_test(slots_cpu devices cornell.pen.json "--res|1080p|--scale|0.5|--spp|32" "-DDEVICE_SETS=cpu+cpu,cpu+cpu,cpu,cpu,cpu" -DSMALL_CHUNKS=1)
pen_render_test(slots_null devices cornell.pen.json "--res|1080p|--spp|256|--backend|null"
                "-DDEVICE_SETS=cpu+cpu,cpu,cpu,cpu+cpu,cpu,cpu,cpu,cpu,cpu,cpu,cpu" -DSMALL_CHUNKS=1)
# 12 chunks for 3 slots (2 in flight each), so the faulty device surely gets work; it fails on its first item.
pen_render_test(fault_throw faults cornell.pen.json "--res|1080p|--scale|0.5|--spp|96" -DREF_DEVICES=cpu
                -DFAULT_DEVICES=cpu,cpu,cpu "-DFAULTS=device=1,after=0,mode=throw" "-DEXPECT_LOG=failed and is removed" -DSMALL_CHUNKS=1)
pen_render_test(fault_hang faults cornell.pen.json "--res|1080p|--spp|256|--backend|null" -DREF_DEVICES=cpu
                -DFAULT_DEVICES=cpu,cpu,cpu "-DFAULTS=device=1,after=2,mode=hang" "-DEXPECT_LOG=is late" -DSMALL_CHUNKS=1
                -DNO_TAIL=1)
# M7 (D-091): the same hang near the end, caught by tail duplication: an idle device copies the stuck item.
pen_render_test(tail_duplicate faults cornell.pen.json "--res|1080p|--spp|256|--backend|null" -DREF_DEVICES=cpu
                -DFAULT_DEVICES=cpu,cpu,cpu "-DFAULTS=device=1,after=2,mode=hang"
                "-DEXPECT_LOG=duplicated to device [^\n]*the first valid result wins.*copy of tile [^\n]* won" -DSMALL_CHUNKS=1)
pen_render_test(spp0_time_limit timelimit cornell.pen.json "--res|1080p|--scale|0.5|--spp|0|--time-limit|5s")
# Killed mid-pass: the single tile holds some of its 8 chunks; resume continues from there, bit for bit.
pen_render_test(resume_chunks resume cornell.pen.json "--res|1080p|--scale|0.5|--spp|128" -DKILL_AFTER_ITEMS=3
                -DSMALL_CHUNKS=1)
# M7 (D-092): a device that cannot take the scene is skipped; the others render the same image.
pen_render_test(skip_device faults cornell.pen.json "--res|1080p|--scale|0.5|--spp|32" -DREF_DEVICES=cpu
                -DFAULT_DEVICES=cpu,cpu "-DFAULTS=device=0,mode=upload" "-DEXPECT_LOG=device 0 [^\n]*is skipped for this render")
set_tests_properties(render.slots_cpu render.fault_throw render.resume_chunks PROPERTIES LABELS "render;gate;slow")
if(UNIX)  # sends a real SIGINT with coreutils `timeout`
  pen_render_test(interrupt interrupt cornell.pen.json "--res|1080p|--spp|16" -DKILL_AFTER=6 -DSMALL_CHUNKS=1)
  set_tests_properties(render.interrupt PROPERTIES LABELS "render;gate;slow")
endif()
# The 20K gate (laptop, 1 spp, ~4 GB film): run with `ctest -L large`; the build scripts skip it.
pen_render_test(large_20k large cornell.pen.json "--res|20k|--spp|1")
set_tests_properties(render.large_20k PROPERTIES LABELS "large" TIMEOUT 3600)

# ---- Network tests (M4): a hosting controller and worker processes on loopback match the solo render ----
add_executable(penumbra-nettest net/nettest.cpp)
function(pen_net_test name port workers scene args)
  add_test(NAME net.${name}
           COMMAND "${CMAKE_COMMAND}" -DPENUMBRA=$<TARGET_FILE:penumbra> -DDIFF=$<TARGET_FILE:penumbra-diff>
                   -DNETTEST=$<TARGET_FILE:penumbra-nettest> -DNETEM=$<TARGET_FILE:penumbra-netem-proxy>
                   -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR} -DNAME=net_${name}
                   -DSCENE=${PEN_SCENES}/${scene} "-DARGS=${args}" -DMODE=network -DPORT=${port}
                   -DWORKERS=${workers} ${ARGN} -P "${PROJECT_SOURCE_DIR}/cmake/RenderCheck.cmake")
  set_tests_properties(net.${name} PROPERTIES LABELS "net;gate" PROCESSORS 8)
endfunction()
# Each test has its own port so ctest -j can run them side by side.
pen_net_test(loopback 7501 3 cornell.pen.json "--res|1080p|--scale|0.5|--spp|32" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--threads|2")
pen_net_test(loopback_local 7502 2 cornell.pen.json "--res|1080p|--scale|0.5|--spp|32" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--threads|2" "-DWORKER_ARGS=--threads|2")
# 3 workers with 4 null devices each, 4K in 135 tiles: many leases in flight, tiles arrive out of order. The
# workers start together: the null backend finishes in seconds, and a worker that comes after the render has
# ended finds no controller.
pen_net_test(loopback_null 7503 3 cornell.pen.json "--res|4k|--spp|256|--backend|null" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--backend|null|--devices|cpu,cpu,cpu,cpu"
             -DWORKER_DELAY_MS=30)
# M4 step 2: a wrong join code or another git commit is turned away while the render goes on; more than 5
# wrong codes in a minute block the address; --approve-workers with no answer rejects the worker.
pen_net_test(wrong_code 7504 1 cornell.pen.json "--res|1080p|--scale|0.5|--spp|32" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--threads|4" -DWRONG_CODE_WORKERS=1
             "-DEXPECT_LOG=failed the encrypted handshake")
pen_net_test(version_mismatch 7505 1 cornell.pen.json "--res|1080p|--scale|0.5|--spp|32" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--threads|4" -DBAD_VERSION_WORKER=1
             "-DEXPECT_LOG=rejected .*0000000badc0de")
pen_net_test(ip_block 7506 1 cornell.pen.json "--res|1080p|--scale|0.5|--spp|128" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--threads|4" -DWRONG_CODE_WORKERS=7
             "-DEXPECT_LOG=blocking 127.0.0.1 for 10 minutes")
pen_net_test(approve_no 7507 1 cornell.pen.json "--res|1080p|--scale|0.5|--spp|16" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--approve-workers|--threads|4" "-DWORKER_ARGS=--threads|2" -DWORKER_EXPECT=6
             "-DEXPECT_LOG=did not approve")
# M4 step 3: workers download the scene package and keep it. A second render finds it in the cache; a worker
# killed mid-download resumes where it stopped (upload capped so the transfer takes a few seconds); a worker
# without room for the scene says so and the render goes on without it. The crashing worker exits after 2 of the
# 4 chunks; the upload limit keeps the uploader's pacing code under test.
pen_net_test(scene_cache 7508 1 cornell.pen.json "--res|1080p|--scale|0.25|--spp|16" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--threads|4" -DRUNS=2
             "-DEXPECT_LOG=has the scene in its cache")
pen_net_test(download_resume 7509 2 bigtex.pen.json "--spp|16" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local|--upload-limit|8M" "-DWORKER_ARGS=--threads|4" -DSHARED_CACHE=1
             -DCRASH_FIRST_AFTER_CHUNKS=2 "-DEXPECT_LOG=resuming at chunk 2 of 4")
pen_net_test(scene_rejected 7510 1 cornell.pen.json "--res|1080p|--scale|0.5|--spp|16" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--threads|4" -DFULL_DISK_WORKER=1
             "-DEXPECT_LOG=full cannot take this scene: not enough disk")
# M4 step 4, the fault table (docs/GUIDE.md "Fault tolerance"): whatever happens to worker 1, worker 2 finishes
# and the image is the solo render, bit for bit. A device fails; the worker process crashes holding leases; a
# device hangs while the worker heartbeats (lease reissued); the worker goes silent (dropped after 30 s without a
# heartbeat); the worker sends corrupt tiles (rejected, kicked after 3); a worker joins mid-render; and a worker
# behind 200 ms latency, 2% loss stalls and a 10 Mbit/s uplink (penumbra-netem-proxy).
set(PEN_NET_12 "--res|1080p|--scale|0.5|--spp|96")  # 12 chunks of 8 samples
pen_net_test(device_fail 7511 2 cornell.pen.json "${PEN_NET_12}" "-DREF_ARGS=--devices|cpu" "-DCONTROLLER_ARGS=--no-local"
             "-DWORKER_ARGS=--threads|4" "-DFIRST_WORKER_ARGS=--devices|cpu,cpu"
             "-DFIRST_WORKER_ENV=PENUMBRA_TEST_FAULTS=device=1,after=0,mode=throw"
             "-DEXPECT_LOG=device 1 of worker 0 failed and is removed")
pen_net_test(device_skipped 7522 2 cornell.pen.json "${PEN_NET_12}" "-DREF_ARGS=--devices|cpu" "-DCONTROLLER_ARGS=--no-local"
             "-DWORKER_ARGS=--threads|4" "-DFIRST_WORKER_ARGS=--devices|cpu,cpu"
             "-DFIRST_WORKER_ENV=PENUMBRA_TEST_FAULTS=device=1,mode=upload"
             "-DEXPECT_LOG=device 1 [^\n]*is skipped for this render")
pen_net_test(worker_crash 7512 2 cornell.pen.json "${PEN_NET_12}" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--threads|4" -DFIRST_WORKER_EXPECT=137
             "-DFIRST_WORKER_ENV=PENUMBRA_TEST_FAULTS=device=0,after=1,mode=exit"
             "-DEXPECT_LOG=worker 0 left .*; [1-9][0-9]* lease\(s\) requeued")
pen_net_test(worker_hang 7513 2 cornell.pen.json "--res|1080p|--spp|256|--backend|null" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--backend|null"
             "-DFIRST_WORKER_ENV=PENUMBRA_TEST_FAULTS=device=0,after=2,mode=hang" "-DEXPECT_LOG=is late" -DNO_TAIL=1)
pen_net_test(tail_duplicate 7521 2 cornell.pen.json "--res|1080p|--spp|256|--backend|null" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--backend|null"
             "-DFIRST_WORKER_ENV=PENUMBRA_TEST_FAULTS=device=0,after=2,mode=hang"
             "-DEXPECT_LOG=duplicated to device [^\n]*the first valid result wins.*copy of tile [^\n]* won")
pen_net_test(worker_silent 7514 2 cornell.pen.json "--res|1080p|--scale|0.5|--spp|64" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--threads|4" -DFIRST_WORKER_EXPECT=7
             "-DFIRST_WORKER_ENV=PENUMBRA_TEST_FAULTS=device=0,after=1,mode=freeze" "-DEXPECT_LOG=heartbeat lost"
             -DNO_TAIL=1 -DWORKER_DELAY_MS=10000)
# Network tests run with the timers x 0.2 (D-106): worker 1 starts at 10 s and goes silent after one item; worker 2
# joins at 20 s, after the controller has dropped worker 1 (6 s), so a fast worker 2 cannot finish the render before
# the heartbeat loss (any speed).
pen_net_test(corrupt_tiles 7515 2 cornell.pen.json "${PEN_NET_12}" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--threads|4" -DFIRST_WORKER_EXPECT=6
             "-DFIRST_WORKER_ENV=PENUMBRA_TEST_FAULTS=device=0,after=0,mode=corrupt" "-DEXPECT_LOG=kicking w1")
pen_net_test(join_mid_render 7516 1 cornell.pen.json "--res|1080p|--scale|0.5|--spp|64" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--threads|2" "-DWORKER_ARGS=--threads|4" -DWORKER_DELAY_MS=5000
             "-DEXPECT_LOG=w1/0 [^\n]*: [1-9][0-9]* items")
pen_net_test(bad_network 7517 1 cornell.pen.json "--res|1080p|--scale|0.5|--spp|32" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--threads|4"
             "-DNETEM_ARGS=--delay-ms|100|--loss|0.02|--up-kbps|10000"
             "-DEXPECT_WORKER_LOG=device 0: [0-9]+ items in flight .round trip [0-9]+ ms.")
set_tests_properties(net.worker_silent PROPERTIES LABELS "net;gate;slow")

# M4 step 5: the controller is killed mid-render (power loss) and resumed from its session file; its --stay
# workers reconnect on their own and finish the image. A --stay worker waits after a render and serves the next
# controller (scene from its cache). Scale: 64 workers with 8 null devices each (512 slots, each item takes
# 300 ms) on one machine.
pen_net_test(stay_resume 7518 2 cornell.pen.json "--res|1080p|--scale|0.5|--spp|256|--checkpoint|10s"
             "-DREF_ARGS=--devices|cpu" "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--threads|4|--stay"
             -DWORKER_EXPECT=daemon -DCONTROLLER_KILL_MS=16000
             "-DEXPECT_LOG=this render was hosting workers.*w1/0 [^\n]*: [1-9][0-9]* items")
pen_net_test(stay_next_render 7519 1 cornell.pen.json "--res|1080p|--scale|0.25|--spp|16" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--threads|4|--stay" -DWORKER_EXPECT=daemon -DSECOND_RENDER=1
             "-DEXPECT_LOG=has the scene in its cache" "-DEXPECT_WORKER_LOG=waiting for the next render")
pen_net_test(scale_64 7520 64 cornell.pen.json "--res|4k|--spp|4096|--backend|null" "-DREF_ARGS=--devices|cpu"
             "-DCONTROLLER_ARGS=--no-local" "-DWORKER_ARGS=--backend|null|--devices|cpu,cpu,cpu,cpu,cpu,cpu,cpu,cpu"
             "-DWORKER_ENV=PENUMBRA_TEST_NULL_DELAY_MS=300" -DWORKER_DELAY_MS=40
             "-DEXPECT_LOG=w64/7 [^\n]*: [1-9][0-9]* items" -DTIME_SCALE=1)
set_tests_properties(net.stay_resume PROPERTIES LABELS "net;gate;slow")
# 64 worker processes: alone on the machine, in real time (with the timers x 0.2 the overloaded controller's round
# trips push the workers' queue depth up, leases go late and are reissued, and it never finishes; an M9 note), and
# skipped on CI's small runners (ctest -LE scale) where it ran past its 900 s limit; the gate runs it on the laptop
# (D-106).
set_tests_properties(net.scale_64 PROPERTIES LABELS "net;gate;slow;scale" RUN_SERIAL TRUE)

# ---- GPU tests (M5, M6): label gpu plus vulkan or cuda, skipped by the build scripts; run with
# scripts/test-gpu.sh (ctest -L gpu; a GPU without ray tracing: ctest -L cuda) ----
# Gate (docs/GUIDE.md M5, M6): each GPU backend matches the CPU at 4,096 spp within relMSE 1e-3. Same backend and
# seed must repeat bit for bit; a mixed GPU + CPU render stays within the tolerance; the furnace stays uniform.
pen_render_test(cutout_cpu runs cutout.pen.json "--res|160x90|--spp|4|--backend|cpu")
function(pen_gpu_test backend name mode scene args)
  pen_render_test(${backend}_${name} ${mode} ${scene} "${args}" ${ARGN})
  set_tests_properties(render.${backend}_${name} PROPERTIES LABELS "gpu;${backend}")
endfunction()
set(_pen_gpu_backends "")
if(PEN_ENABLE_VULKAN)
  list(APPEND _pen_gpu_backends vulkan)
endif()
if(PEN_ENABLE_CUDA)
  list(APPEND _pen_gpu_backends cuda)
endif()
foreach(_b IN LISTS _pen_gpu_backends)
  pen_gpu_test(${_b} golden_cornell reference cornell.pen.json "--res|256x256|--spp|4096|--backend|${_b}"
               -DREFERENCE=${PROJECT_SOURCE_DIR}/tests/golden/cornell_256_4096spp.exr -DMAXRELMSE=1e-3)
  pen_gpu_test(${_b} spheres_vs_cpu pair spheres.pen.json "--res|320x180|--spp|4096" "-DARGS_A=--backend|cpu"
               "-DARGS_B=--backend|${_b}" -DMAXRELMSE=1e-3)
  pen_gpu_test(${_b} bigtex_vs_cpu pair bigtex.pen.json "--res|240x240|--spp|1024" "-DARGS_A=--backend|cpu"
               "-DARGS_B=--backend|${_b}" -DMAXRELMSE=1e-3)
  pen_gpu_test(${_b} cutout_vs_cpu pair cutout.pen.json "--res|320x180|--spp|1024" "-DARGS_A=--backend|cpu"
               "-DARGS_B=--backend|${_b}" -DMAXRELMSE=1e-3)
  pen_gpu_test(${_b} furnace_diffuse uniform furnace_diffuse.pen.json "--res|32x32|--spp|16384|--backend|${_b}"
               -DTOLERANCE=0.005)
  pen_gpu_test(${_b} furnace_glass uniform furnace_glass.pen.json "--spp|256|--backend|${_b}" -DTOLERANCE=0.005)
  pen_gpu_test(${_b} repeatable devices spheres.pen.json "--res|320x180|--spp|64|--backend|${_b}" "-DDEVICE_SETS=0+0")
  # Small chunks, so the frame is many items and both devices get some (the log must show it). No tail
  # duplication: a fast GPU would otherwise also finish the CPU's last items and leave it with none (D-091).
  pen_gpu_test(${_b} hybrid pair cornell.pen.json "--res|1080p|--scale|0.25|--spp|1024" "-DARGS_A=--backend|cpu"
               "-DARGS_B=--backend|${_b}|--devices|0,cpu" -DMAXRELMSE=1e-3 -DSMALL_CHUNKS=1 -DNO_TAIL=1
               "-DEXPECT_LOG=local/0 [^\n]*: [1-9][0-9]* items.*local/1 [^\n]*: [1-9][0-9]* items")
  pen_gpu_test(${_b} ao_vs_cpu pair spheres.pen.json "--res|160x90|--spp|256|--integrator|ao" "-DARGS_A=--backend|cpu"
               "-DARGS_B=--backend|${_b}" -DMAXRELMSE=1e-3)
  pen_gpu_test(${_b} debug_normal runs spheres.pen.json "--res|160x90|--spp|1|--integrator|debug:normal|--backend|${_b}")
  # M7, several GPUs (skipped on a machine with one): all GPUs give the same bits as one GPU of the same backend,
  # and a GPU that fails or hangs mid-render changes nothing (DECISIONS D-091, D-094).
  pen_gpu_test(${_b} multi_gpu devices cornell.pen.json "--res|1080p|--scale|0.5|--spp|256|--backend|${_b}"
               "-DDEVICE_SETS=0+gpu" -DSMALL_CHUNKS=1 -DMIN_GPUS=2)
  pen_gpu_test(${_b} multi_gpu_fail faults cornell.pen.json "--res|1080p|--scale|0.5|--spp|256|--backend|${_b}"
               -DREF_DEVICES=0 -DFAULT_DEVICES=gpu "-DFAULTS=device=1,after=1,mode=throw"
               "-DEXPECT_LOG=failed and is removed" -DSMALL_CHUNKS=1 -DMIN_GPUS=2)
  pen_gpu_test(${_b} multi_gpu_hang faults cornell.pen.json "--res|1080p|--scale|0.5|--spp|256|--backend|${_b}"
               -DREF_DEVICES=0 -DFAULT_DEVICES=gpu "-DFAULTS=device=1,after=1,mode=hang"
               "-DEXPECT_LOG=duplicated to device" -DSMALL_CHUNKS=1 -DMIN_GPUS=2)
  set_tests_properties(render.${_b}_multi_gpu render.${_b}_multi_gpu_fail render.${_b}_multi_gpu_hang
                       PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
  # M8 (D-108): the memory tier ladder on bigtex (one 2048x2048 texture, ~17 MB on the GPU). Tier 1 moves the same
  # bytes to RAM: bit-identical. Tier 2 halves the texture once (10 MB of VRAM, 30 MB of RAM less the scene itself):
  # the CPU halving it the same way must agree. Tier 3 and a forbidden downsize skip the GPU; the CPU finishes, so
  # the image is the CPU's. An injected out-of-memory at tier 0 lands at tier 1, same bits.
  pen_gpu_test(${_b} tier1_bits pair bigtex.pen.json "--res|240x240|--spp|64" "-DARGS_A=--backend|${_b}"
               "-DARGS_B=--backend|${_b}|--vram-limit|10M" -DMAXRELMSE=0 "-DEXPECT_LOG=tier 1, scene")
  pen_gpu_test(${_b} tier2_vs_cpu pair bigtex.pen.json "--res|240x240|--spp|1024" "-DARGS_A=--backend|cpu"
               "-DARGS_B=--backend|${_b}|--vram-limit|10M|--ram-limit|30M" -DMAXRELMSE=1e-3 -DTEXTURE_HALVINGS=1
               "-DEXPECT_LOG=tier 2, [^\n]*1 texture.s. downsized .at most 1/2 per side.")
  pen_gpu_test(${_b} tier3_skip pair bigtex.pen.json "--res|240x240|--spp|16" "-DARGS_A=--backend|cpu"
               "-DARGS_B=--devices|0:${_b},cpu|--vram-limit|1K" -DMAXRELMSE=0 "-DEXPECT_LOG=memory tier 3")
  pen_gpu_test(${_b} no_downsize pair bigtex.pen.json "--res|240x240|--spp|16" "-DARGS_A=--backend|cpu"
               "-DARGS_B=--devices|0:${_b},cpu|--vram-limit|10M|--ram-limit|30M|--no-texture-downsize" -DMAXRELMSE=0
               "-DEXPECT_LOG=memory tier 3[^\n]*--no-texture-downsize")
  pen_gpu_test(${_b} oom_retry faults bigtex.pen.json "--res|240x240|--spp|64|--backend|${_b}" -DREF_DEVICES=0
               -DFAULT_DEVICES=0 "-DFAULTS=device=0,mode=oom" "-DEXPECT_LOG=out of memory at tier 0")
endforeach()
if(PEN_ENABLE_CUDA)
  # Traversal step counts exist only in the software BVH.
  pen_gpu_test(cuda debug_bvh runs spheres.pen.json "--res|160x90|--spp|1|--integrator|debug:bvh|--backend|cuda")
endif()
if(PEN_ENABLE_VULKAN AND PEN_ENABLE_CUDA)
  # The two GPU backends share the shading code (DECISIONS D-085); on one GPU they must agree closely.
  pen_render_test(gpu_cuda_vs_vulkan pair cutout.pen.json "--res|320x180|--spp|1024" "-DARGS_A=--backend|vulkan"
                  "-DARGS_B=--backend|cuda" -DMAXRELMSE=1e-3)
  set_tests_properties(render.gpu_cuda_vs_vulkan PROPERTIES LABELS "gpu;vulkan;cuda")
  # M7 (D-090): backends mixed on one host; every device renders items. One GPU listed twice must give the right
  # image, but the driver time-slices the two contexts and may starve one, so no per-device check there.
  pen_render_test(gpu_mixed_one_gpu pair cornell.pen.json "--res|1080p|--scale|0.25|--spp|1024" "-DARGS_A=--backend|cpu"
                  "-DARGS_B=--devices|0:vulkan,0:cuda,cpu" -DMAXRELMSE=1e-3 -DSMALL_CHUNKS=1)
  pen_render_test(gpu_mixed_multi pair cornell.pen.json "--res|1080p|--scale|0.25|--spp|1024" "-DARGS_A=--backend|cpu"
                  "-DARGS_B=--devices|0:vulkan,1:cuda,cpu" -DMAXRELMSE=1e-3 -DSMALL_CHUNKS=1 -DMIN_GPUS=2 -DNO_TAIL=1
                  "-DEXPECT_LOG=local/0 [^\n]*: [1-9][0-9]* items.*local/1 [^\n]*: [1-9][0-9]* items.*local/2 [^\n]*: [1-9][0-9]* items")
  set_tests_properties(render.gpu_mixed_one_gpu render.gpu_mixed_multi PROPERTIES LABELS "gpu;vulkan;cuda")
  set_tests_properties(render.gpu_mixed_multi PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
endif()
if(_pen_gpu_backends)
  # Every Slang shading function against its C++ original on 65,536 inputs (DECISIONS D-077).
  add_executable(penumbra-gpu-parity gpu/parity.cpp)
  target_link_libraries(penumbra-gpu-parity PRIVATE penumbra_backends)
  foreach(_b IN LISTS _pen_gpu_backends)
    add_test(NAME gpu.parity_${_b} COMMAND penumbra-gpu-parity --backend ${_b})
    set_tests_properties(gpu.parity_${_b} PROPERTIES LABELS "gpu;${_b}")
  endforeach()
endif()

if(NOT EXISTS "${PROJECT_SOURCE_DIR}/tests/golden/cornell_256_4096spp.exr")
  message(STATUS "tests/golden/cornell_256_4096spp.exr not found: render.golden_cornell is disabled")
  set_tests_properties(render.golden_cornell PROPERTIES DISABLED TRUE)
  foreach(_b IN LISTS _pen_gpu_backends)
    set_tests_properties(render.${_b}_golden_cornell PROPERTIES DISABLED TRUE)
  endforeach()
endif()

# ---- Repository checks ----
add_test(NAME docs.cli_reference
         COMMAND "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:penumbra> -DREADME=${PROJECT_SOURCE_DIR}/README.md -DMODE=check
                 -P "${PROJECT_SOURCE_DIR}/cmake/CliDocs.cmake")
add_test(NAME lint.layering
         COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P "${PROJECT_SOURCE_DIR}/cmake/CheckLayering.cmake")
set_tests_properties(docs.cli_reference lint.layering PROPERTIES LABELS lint)
# The sample scenes in scenes/ (for a quick render after cloning, D-096) must render, and must stay what
# penumbra-scenegen writes. spheres.gltf is not compared: its vertices come from sin/cos, whose last bits differ
# between MSVC and GCC, so the generated copy differs per OS.
foreach(_f cornell.gltf cornell.pen.json spheres.pen.json sky.hdr cutout.gltf cutout.pen.json cutout_checker.png)
  add_test(NAME docs.sample_scene_${_f}
           COMMAND "${CMAKE_COMMAND}" -E compare_files "${PROJECT_SOURCE_DIR}/scenes/${_f}" "${PEN_SCENES}/${_f}")
  set_tests_properties(docs.sample_scene_${_f} PROPERTIES LABELS lint)
endforeach()
foreach(_s cornell spheres cutout)
  add_test(NAME render.sample_${_s}
           COMMAND "${CMAKE_COMMAND}" -DPENUMBRA=$<TARGET_FILE:penumbra> -DDIFF=$<TARGET_FILE:penumbra-diff>
                   -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR} -DNAME=sample_${_s}
                   -DSCENE=${PROJECT_SOURCE_DIR}/scenes/${_s}.pen.json "-DARGS=--res|160x90|--spp|4" -DMODE=runs
                   -P "${PROJECT_SOURCE_DIR}/cmake/RenderCheck.cmake")
  set_tests_properties(render.sample_${_s} PROPERTIES LABELS "render")
endforeach()
