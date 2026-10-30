# Intel Open Image Denoise 2 (M11, DECISIONS D-125, D-128): vcpkg has no port, so the official release binaries
# are downloaded once at configure time (pinned version, SHA-256 checked) into the build folder. Sets
# PEN_OIDN_RUNTIME: the shared libraries penumbra needs next to it (core, CPU and CUDA devices, TBB; the AMD and
# Intel GPU devices are left out).
set(PEN_OIDN_VERSION 2.5.1)
if(WIN32)
  set(_pen_oidn_name "oidn-${PEN_OIDN_VERSION}.x64.windows")
  set(_pen_oidn_archive "${_pen_oidn_name}.zip")
  set(_pen_oidn_sha256 f11f91bc072a5e3a564515724cb72ab8fcfbc445c84a84c197ff9b16cc01396f)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(_pen_oidn_name "oidn-${PEN_OIDN_VERSION}.x86_64.linux")
  set(_pen_oidn_archive "${_pen_oidn_name}.tar.gz")
  set(_pen_oidn_sha256 743c3e2aff8c220d5d70fe6cb970fb3d36f2702d2693c61d1d148e404cf37cd6)
else()
  message(FATAL_ERROR "No Open Image Denoise release for this platform; configure with -DPEN_ENABLE_OIDN=OFF")
endif()

set(_pen_oidn_dir "${CMAKE_BINARY_DIR}/_deps/${_pen_oidn_name}")
if(NOT EXISTS "${_pen_oidn_dir}/include/OpenImageDenoise/oidn.hpp")
  set(_url "https://github.com/RenderKit/oidn/releases/download/v${PEN_OIDN_VERSION}/${_pen_oidn_archive}")
  set(_file "${CMAKE_BINARY_DIR}/_deps/${_pen_oidn_archive}")
  message(STATUS "Downloading Open Image Denoise ${PEN_OIDN_VERSION} (about 60 MB) from ${_url}")
  file(DOWNLOAD "${_url}" "${_file}" EXPECTED_HASH SHA256=${_pen_oidn_sha256} STATUS _status TLS_VERIFY ON)
  list(GET _status 0 _code)
  if(NOT _code EQUAL 0)
    list(GET _status 1 _why)
    message(FATAL_ERROR "Could not download Open Image Denoise (${_why}). Check the internet connection, or "
                        "configure with -DPEN_ENABLE_OIDN=OFF to build without denoising.")
  endif()
  file(ARCHIVE_EXTRACT INPUT "${_file}" DESTINATION "${CMAKE_BINARY_DIR}/_deps")
  file(REMOVE "${_file}")
endif()

find_package(OpenImageDenoise ${PEN_OIDN_VERSION} EXACT CONFIG REQUIRED PATHS "${_pen_oidn_dir}" NO_DEFAULT_PATH)
if(WIN32)
  set(PEN_OIDN_RUNTIME
      "${_pen_oidn_dir}/bin/OpenImageDenoise.dll" "${_pen_oidn_dir}/bin/OpenImageDenoise_core.dll"
      "${_pen_oidn_dir}/bin/OpenImageDenoise_device_cpu.dll" "${_pen_oidn_dir}/bin/OpenImageDenoise_device_cuda.dll"
      "${_pen_oidn_dir}/bin/tbb12.dll")
else()
  file(GLOB PEN_OIDN_RUNTIME
       "${_pen_oidn_dir}/lib/libOpenImageDenoise.so*" "${_pen_oidn_dir}/lib/libOpenImageDenoise_core.so*"
       "${_pen_oidn_dir}/lib/libOpenImageDenoise_device_cpu.so*" "${_pen_oidn_dir}/lib/libOpenImageDenoise_device_cuda.so*"
       "${_pen_oidn_dir}/lib/libtbb.so*")
endif()
message(STATUS "Open Image Denoise ${PEN_OIDN_VERSION}: ${_pen_oidn_dir}")

# Copies the runtime libraries next to `target` after each build (Windows looks for DLLs there; on Linux the
# binaries look in their own folder first, $ORIGIN).
function(pen_copy_oidn_runtime target)
  add_custom_command(TARGET ${target} POST_BUILD
                     COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${PEN_OIDN_RUNTIME} "$<TARGET_FILE_DIR:${target}>"
                     VERBATIM)
  if(NOT WIN32)
    # DT_RPATH, not the newer DT_RUNPATH: a RUNPATH applies to the binary's own dependencies only, so
    # libOpenImageDenoise.so.2 was found but its libOpenImageDenoise_core.so was not (D-128).
    set_target_properties(${target} PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
    target_link_options(${target} PRIVATE "LINKER:--disable-new-dtags")
  endif()
endfunction()
