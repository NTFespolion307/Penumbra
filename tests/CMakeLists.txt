find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

set(PEN_TEST_DATA "${CMAKE_CURRENT_SOURCE_DIR}/data")

# ---- Unit tests ----
add_executable(penumbra_unit_tests
  unit/test_bsdf.cpp
  unit/test_cli.cpp
  unit/test_config.cpp
  unit/test_parse.cpp
  unit/test_presets.cpp
  unit/test_render_path.cpp
  unit/test_sampling.cpp
  unit/test_settings.cpp)
target_link_libraries(penumbra_unit_tests PRIVATE penumbra_cli penumbra_controller Catch2::Catch2WithMain)
target_compile_definitions(penumbra_unit_tests PRIVATE PEN_TEST_DATA_DIR="${PEN_TEST_DATA}")
catch_discover_tests(penumbra_unit_tests PROPERTIES LABELS unit)

# ---- Smoke tests: run the real binary and check exit codes and messages ----
set(PEN_SMOKE_COMMON "--config|${PEN_TEST_DATA}/empty-config.json|--log-file|${CMAKE_CURRENT_BINARY_DIR}/smoke.log")
function(pen_smoke name expect args)
  set(cmd "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:penumbra> "-DARGS=${args}" -DEXPECT=${expect}
          -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR})
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
pen_smoke(empty_scene_renders 0 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/empty.gltf|--res|240p|--spp|1|-o|empty.png"
          "426x240.*no lights")
pen_smoke(pen_json 0 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/scene.pen.json|--scale|0.05|--spp|1|-o|pen.png"
          "128x72 \\(2k at scale 0.05\\).*camera 'Camera.001'")
pen_smoke(exr_half 0 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/scene.pen.json|--res|64x36|--spp|1|-o|half.exr" "wrote")
pen_smoke(exr_float 0 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/scene.pen.json|--res|64x36|--spp|1|--bit-depth|32|-o|float.exr" "wrote")
pen_smoke(png_16bit 0 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/scene.pen.json|--res|64x36|--spp|1|--bit-depth|16|-o|deep.png" "wrote")
pen_smoke(jpeg_not_yet 1 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/empty.gltf|-o|out.jpg" "built in milestone M2")
pen_smoke(gpu_backend_not_yet 3 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/empty.gltf|--backend|vulkan|-o|x.png" "M5")
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
  set_tests_properties(render.${name} PROPERTIES LABELS "render;gate")
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
                -DREFERENCE=${PROJECT_SOURCE_DIR}/tests/golden/cornell_256_4096spp.exr -DMAXRELMSE=2e-3)
# Every material, the HDRI, AO and the debug views render.
pen_render_test(spheres runs spheres.pen.json "--res|160x90|--spp|16")
pen_render_test(ao runs spheres.pen.json "--res|160x90|--spp|4|--integrator|ao")
pen_render_test(debug_normal runs spheres.pen.json "--res|160x90|--spp|1|--integrator|debug:normal")
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/tests/golden/cornell_256_4096spp.exr")
  message(STATUS "tests/golden/cornell_256_4096spp.exr not found: render.golden_cornell is disabled")
  set_tests_properties(render.golden_cornell PROPERTIES DISABLED TRUE)
endif()

# ---- Repository checks ----
add_test(NAME docs.cli_reference
         COMMAND "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:penumbra> -DREADME=${PROJECT_SOURCE_DIR}/README.md -DMODE=check
                 -P "${PROJECT_SOURCE_DIR}/cmake/CliDocs.cmake")
add_test(NAME lint.layering
         COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P "${PROJECT_SOURCE_DIR}/cmake/CheckLayering.cmake")
set_tests_properties(docs.cli_reference lint.layering PROPERTIES LABELS lint)
