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

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Penumbra needs a 64-bit compiler. On Windows, build from an x64 developer shell "
                      "(scripts/build.ps1 sets this up for you).")
endif()

# ---- Feature options (see docs/GUIDE.md "Build, dependencies and platforms") ----
option(PEN_ENABLE_VULKAN "Vulkan RT backend and UI swapchain (built from M5)" ON)
option(PEN_ENABLE_OPTIX "OptiX backend and denoiser (optional, M13)" OFF)
option(PEN_ENABLE_UI "Dear ImGui user interface (built from M10)" ON)
if(NOT DEFINED PEN_ENABLE_CUDA)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    set(_pen_cuda_default ON)
  else()
    set(_pen_cuda_default OFF)
  endif()
  option(PEN_ENABLE_CUDA "CUDA compute backend (built from M6); ON if the CUDA toolkit is found" ${_pen_cuda_default})
endif()
set(PEN_CUDA_ARCHS "75;80;86;89;90;120" CACHE STRING "CUDA architectures to compile for")

# ---- Compiler settings ----
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Git QUIET)

add_subdirectory(core)
add_subdirectory(net)
add_subdirectory(backends)
add_subdirectory(controller)
add_subdirectory(worker)
add_subdirectory(apps/penumbra)

if(PEN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "Penumbra ${PROJECT_VERSION}: Vulkan=${PEN_ENABLE_VULKAN} CUDA=${PEN_ENABLE_CUDA} "
               "OptiX=${PEN_ENABLE_OPTIX} UI=${PEN_ENABLE_UI} tests=${PEN_BUILD_TESTS} "
               "(GPU backends and UI are not built yet: M5, M6, M10)")
