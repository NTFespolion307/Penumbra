# core: math, scene, settings, config, sampling, film, output, package, memory, log.
# No GPU API or socket headers here (checked by the "layering" test).

set(PEN_VERSION_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/version_info.cpp")
add_custom_target(penumbra_git_version
  COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DOUTPUT=${PEN_VERSION_CPP}
          -DVERSION=${PROJECT_VERSION} -DGIT=${GIT_EXECUTABLE} -P "${PROJECT_SOURCE_DIR}/cmake/GitVersion.cmake"
  BYPRODUCTS "${PEN_VERSION_CPP}"
  COMMENT "Checking git version"
  VERBATIM)

add_library(penumbra_core STATIC
  version.cpp
  "${PEN_VERSION_CPP}"
  config/config.cpp
  log/log.cpp
  settings/parse.cpp
  settings/presets.cpp
  settings/schema.cpp
  system/paths.cpp
  system/sysinfo.cpp)
add_dependencies(penumbra_core penumbra_git_version)
target_include_directories(penumbra_core PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(penumbra_core PUBLIC fmt::fmt nlohmann_json::nlohmann_json spdlog::spdlog)
if(WIN32)
  target_link_libraries(penumbra_core PRIVATE shell32 ole32 advapi32)
endif()
