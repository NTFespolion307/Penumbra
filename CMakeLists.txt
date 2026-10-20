cmake_minimum_required(VERSION 3.26)

# Tests are a vcpkg manifest feature, so Catch2 is only fetched when tests are built.
option(PEN_BUILD_TESTS "Build unit, smoke and lint tests" ON)
if(PEN_BUILD_TESTS)
  list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()

project(Penumbra VERSION 0.1.0 DESCRIPTION "Distributed multi-GPU path tracer" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# A build folder remembers the vcpkg it was first configured with. If the toolchain now points at another vcpkg
# (e.g. the copy bundled with Visual Studio was picked up once), stop instead of silently mixing them.
if(DEFINED Z_VCPKG_ROOT_DIR AND CMAKE_TOOLCHAIN_FILE)
  cmake_path(GET CMAKE_TOOLCHAIN_FILE PARENT_PATH _pen_tc_dir)  # .../scripts/buildsystems
  cmake_path(GET _pen_tc_dir PARENT_PATH _pen_tc_dir)
  cmake_path(GET _pen_tc_dir PARENT_PATH _pen_expected_root)
  file(REAL_PATH "${_pen_expected_root}" _pen_expected_root)
  file(REAL_PATH "${Z_VCPKG_ROOT_DIR}" _pen_used_root)
  string(TOLOWER "${_pen_expected_root}" _pen_a)
  string(TOLOWER "${_pen_used_root}" _pen_b)
  if(NOT _pen_a STREQUAL _pen_b)
    message(FATAL_ERROR "This build folder was configured with the vcpkg in '${_pen_used_root}', but VCPKG_ROOT now "
                        "points to '${_pen_expected_root}'. Delete the build folder (or run the build script with "
                        "-Clean / --clean) and configure again.")
  endif()
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Penumbra needs a 64-bit compiler. On Windows, build from an x64 developer shell "
                      "(scripts/build.ps1 sets this up for you).")
endif()

# ---- Feature options (see docs/GUIDE.md "Build, dependencies and platforms") ----
option(PEN_ENABLE_VULKAN "Vulkan RT backend and UI swapchain (built from M5)" ON)
option(PEN_ENABLE_OPTIX "OptiX backend and denoiser (optional, M13)" OFF)
option(PEN_ENABLE_UI "Dear ImGui user interface (built from M10)" ON)
if(NOT DEFINED PEN_ENABLE_CUDA)
  # ON when a CUDA toolkit (nvcc) is found: on PATH, through CUDA_PATH (the Windows installer sets it) or in
  # /usr/local/cuda. Nothing links against CUDA; the driver is loaded at run time (DECISIONS D-087).
  find_package(CUDAToolkit QUIET)
  if(CUDAToolkit_FOUND AND CUDAToolkit_NVCC_EXECUTABLE)
    set(_pen_cuda_default ON)
  else()
    set(_pen_cuda_default OFF)
  endif()
  option(PEN_ENABLE_CUDA "CUDA compute backend (built from M6); ON if the CUDA toolkit is found" ${_pen_cuda_default})
endif()
set(PEN_CUDA_ARCHS "75;80;86;89;90;120" CACHE STRING
    "CUDA architectures to compile for (machine code for each; the oldest also as PTX for newer GPUs)")

# ---- Compiler settings ----
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  # No fused multiply-add contraction: the same code must produce the same bits on every host.
  add_compile_options(-Wall -Wextra -Wpedantic -ffp-contract=off)
endif()

find_package(CLI11 CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)  # GCC 12 has no <format>; fmt is also spdlog's formatter (DECISIONS D-018)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(embree 4 CONFIG REQUIRED)
find_package(TBB CONFIG REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)
find_package(OpenEXR CONFIG REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(TIFF REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(BLAKE3 CONFIG REQUIRED)
find_path(CGLTF_INCLUDE_DIRS "cgltf.h" REQUIRED)
find_path(STB_INCLUDE_DIRS "stb_image.h" REQUIRED)
find_package(Git QUIET)

add_subdirectory(core)
add_subdirectory(net)
add_subdirectory(backends)
add_subdirectory(worker)
add_subdirectory(controller)
add_subdirectory(apps/penumbra)
add_subdirectory(tools)

if(PEN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "Penumbra ${PROJECT_VERSION}: Vulkan=${PEN_ENABLE_VULKAN} CUDA=${PEN_ENABLE_CUDA} "
               "OptiX=${PEN_ENABLE_OPTIX} UI=${PEN_ENABLE_UI} tests=${PEN_BUILD_TESTS} (the UI is built from M10)")
