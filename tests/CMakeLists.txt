find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

set(PEN_TEST_DATA "${CMAKE_CURRENT_SOURCE_DIR}/data")

# ---- Unit tests ----
add_executable(penumbra_unit_tests
  unit/test_cli.cpp
  unit/test_config.cpp
  unit/test_parse.cpp
  unit/test_presets.cpp
  unit/test_settings.cpp)
target_link_libraries(penumbra_unit_tests PRIVATE penumbra_cli penumbra_core Catch2::Catch2WithMain)
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
pen_smoke(terminal_not_yet 1 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/empty.gltf|--res|4k|-o|out.exr"
          "3840x2160.*milestone M1")
pen_smoke(pen_json 1 "${PEN_SMOKE_COMMON}|--terminal|${PEN_TEST_DATA}/scene.pen.json" "2560x1440.*512 spp.*milestone M1")

# ---- Repository checks ----
add_test(NAME docs.cli_reference
         COMMAND "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:penumbra> -DREADME=${PROJECT_SOURCE_DIR}/README.md -DMODE=check
                 -P "${PROJECT_SOURCE_DIR}/cmake/CliDocs.cmake")
add_test(NAME lint.layering
         COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -P "${PROJECT_SOURCE_DIR}/cmake/CheckLayering.cmake")
set_tests_properties(docs.cli_reference lint.layering PROPERTIES LABELS lint)
