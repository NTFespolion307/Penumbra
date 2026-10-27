# core: math, scene, settings, config, sampling, shading, film, output, log.
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
  film/film.cpp
  film/session.cpp
  image/image.cpp
  image/image_io.cpp
  log/log.cpp
  output/output.cpp
  output/writers.cpp
  package/package.cpp
  render/camera.cpp
  render/lights.cpp
  render/memory_placement.cpp
  render/surface.cpp
  sampling/alias_table.cpp
  scene/gltf_loader.cpp
  scene/obj_loader.cpp
  scene/scene.cpp
  scene/scene_loader.cpp
  settings/parse.cpp
  settings/presets.cpp
  settings/schema.cpp
  settings/serialize.cpp
  shading/bsdf.cpp
  system/file.cpp
  system/hash.cpp
  system/paths.cpp
  system/sysinfo.cpp
  system/test_hooks.cpp)
add_dependencies(penumbra_core penumbra_git_version)
target_include_directories(penumbra_core PUBLIC "${PROJECT_SOURCE_DIR}")
target_include_directories(penumbra_core SYSTEM PRIVATE ${CGLTF_INCLUDE_DIRS} ${STB_INCLUDE_DIRS})
target_link_libraries(penumbra_core
  PUBLIC fmt::fmt nlohmann_json::nlohmann_json spdlog::spdlog
  PRIVATE tinyobjloader::tinyobjloader OpenEXR::OpenEXR PNG::PNG JPEG::JPEG TIFF::TIFF WebP::webp BLAKE3::blake3)
if(WIN32)
  target_link_libraries(penumbra_core PRIVATE shell32 ole32 advapi32)
endif()
