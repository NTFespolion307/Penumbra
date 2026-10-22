# backends: IBackend implementations: cpu/ (Embree, M1), null/ (test only), vulkan/ (M5), cuda/ (M6), optix/ (M13),
# and gpu/ (scene data shared by the GPU backends). The only place that may include Vulkan, CUDA, OptiX or Embree.
add_library(penumbra_backends STATIC cpu/cpu_backend.cpp null/null_backend.cpp gpu/gpu_scene.cpp)
target_link_libraries(penumbra_backends PUBLIC penumbra_core PRIVATE embree TBB::tbb)

# Third-party code compiled here has its own warnings, which are not ours.
set(_pen_no_warnings "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")

if(PEN_ENABLE_VULKAN OR PEN_ENABLE_CUDA)
  # The shaders (shaders/*.slang) compile at build time with the pinned slangc from vcpkg (a host tool): to SPIR-V
  # for Vulkan and to CUDA C++ for CUDA (DECISIONS D-076, D-085).
  file(GLOB _pen_slangc_dirs "${VCPKG_INSTALLED_DIR}/*/tools/shader-slang")
  find_program(PEN_SLANGC slangc REQUIRED HINTS ${_pen_slangc_dirs} NO_DEFAULT_PATH)
  file(GLOB PEN_SHADER_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/shaders/*.slang")
  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
  file(MAKE_DIRECTORY "${_gen}")
endif()

if(PEN_ENABLE_VULKAN)
  # One SPIR-V module per entry file, embedded in the binary.
  find_package(volk CONFIG REQUIRED)
  find_package(VulkanMemoryAllocator CONFIG REQUIRED)
  foreach(_shader pathtrace parity)
    if(_shader STREQUAL "pathtrace")
      set(_name kPathtraceSpv)
    else()
      set(_name kParitySpv)
    endif()
    add_custom_command(
      OUTPUT "${_gen}/${_shader}.spv"
      COMMAND "${PEN_SLANGC}" "${PROJECT_SOURCE_DIR}/shaders/${_shader}.slang" -target spirv -profile spirv_1_5
              -fvk-use-scalar-layout -fvk-use-entrypoint-name -O3 -o "${_gen}/${_shader}.spv"
      DEPENDS ${PEN_SHADER_SOURCES}
      COMMENT "Compiling ${_shader}.slang (Slang -> SPIR-V)"
      VERBATIM)
    add_custom_command(
      OUTPUT "${_gen}/${_shader}_spv.cpp"
      COMMAND "${CMAKE_COMMAND}" -DINPUT=${_gen}/${_shader}.spv -DOUTPUT=${_gen}/${_shader}_spv.cpp -DNAME=${_name}
              -P "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
      DEPENDS "${_gen}/${_shader}.spv" "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
      VERBATIM)
    target_sources(penumbra_backends PRIVATE "${_gen}/${_shader}_spv.cpp")
  endforeach()
  target_sources(penumbra_backends PRIVATE vulkan/vulkan_backend.cpp vulkan/vma.cpp)
  target_link_libraries(penumbra_backends PRIVATE volk::volk volk::volk_headers GPUOpen::VulkanMemoryAllocator)
  target_compile_definitions(penumbra_backends PUBLIC PEN_HAS_VULKAN=1)
  set_source_files_properties(vulkan/vma.cpp PROPERTIES COMPILE_OPTIONS "${_pen_no_warnings}")
endif()

if(PEN_ENABLE_CUDA)
  # Slang -> CUDA C++ -> nvcc -> one fatbin per kernel, embedded in the binary; the CUDA driver is loaded at run
  # time, so nothing links against CUDA (DECISIONS D-087).
  find_package(CUDAToolkit REQUIRED)

  # Machine code for every architecture in PEN_CUDA_ARCHS this nvcc supports, all from ONE PTX of the oldest
  # architecture: every GPU generation then runs the same instructions (same bits), and the embedded PTX lets the
  # driver compile for GPUs newer than the list.
  execute_process(COMMAND "${CUDAToolkit_NVCC_EXECUTABLE}" --list-gpu-code OUTPUT_VARIABLE _pen_nvcc_codes
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(_pen_archs "")
  foreach(_a IN LISTS PEN_CUDA_ARCHS)
    if(_pen_nvcc_codes MATCHES "sm_${_a}([^0-9]|$)")
      list(APPEND _pen_archs ${_a})
    else()
      message(WARNING "CUDA ${CUDAToolkit_VERSION} cannot compile for sm_${_a}; leaving it out of PEN_CUDA_ARCHS")
    endif()
  endforeach()
  if(NOT _pen_archs)
    message(FATAL_ERROR "PEN_CUDA_ARCHS (${PEN_CUDA_ARCHS}): none of these architectures is supported by CUDA "
                        "${CUDAToolkit_VERSION}")
  endif()
  list(SORT _pen_archs COMPARE NATURAL)
  list(GET _pen_archs 0 _pen_ptx)
  set(_pen_codes "")
  foreach(_a IN LISTS _pen_archs)
    string(APPEND _pen_codes "sm_${_a},")
  endforeach()
  set(_pen_gencode "arch=compute_${_pen_ptx},code=[${_pen_codes}compute_${_pen_ptx}]")
  string(REPLACE ";" "," _pen_sass "${_pen_archs}")
  message(STATUS "CUDA ${CUDAToolkit_VERSION}: kernels for sm_${_pen_sass} (from compute_${_pen_ptx} PTX)")

  set(_pen_nvcc_host -ccbin "${CMAKE_CXX_COMPILER}")
  if(MSVC)
    # nvcc only preprocesses with the host compiler here (device code only), so a Visual Studio newer than the
    # toolkit knows is fine.
    list(APPEND _pen_nvcc_host -allow-unsupported-compiler)
  endif()

  add_executable(pen-embed-binary "${PROJECT_SOURCE_DIR}/tools/embed_binary.cpp")
  foreach(_shader pathtrace_cuda parity)
    if(_shader STREQUAL "pathtrace_cuda")
      set(_name kPathtraceFatbin)
      set(_layout pathtrace)
      set(_src "${PROJECT_SOURCE_DIR}/shaders/pathtrace_cuda.slang")
    else()
      set(_name kParityFatbin)
      set(_layout parity)
      set(_src "${PROJECT_SOURCE_DIR}/shaders/parity.slang")
    endif()
    add_custom_command(
      OUTPUT "${_gen}/${_shader}.cu" "${_gen}/${_shader}.json"
      COMMAND "${PEN_SLANGC}" "${_src}" -target cuda -O3 -warnings-disable 41035 -o "${_gen}/${_shader}.cu"
              -reflection-json "${_gen}/${_shader}.json"
      DEPENDS ${PEN_SHADER_SOURCES}
      COMMENT "Compiling ${_shader} (Slang -> CUDA C++)"
      VERBATIM)
    add_custom_command(
      OUTPUT "${_gen}/${_layout}_cuda_layout.h"
      COMMAND "${CMAKE_COMMAND}" -DINPUT=${_gen}/${_shader}.json -DOUTPUT=${_gen}/${_layout}_cuda_layout.h
              -DNAMESPACE=${_layout} -P "${PROJECT_SOURCE_DIR}/cmake/CudaLayout.cmake"
      DEPENDS "${_gen}/${_shader}.json" "${PROJECT_SOURCE_DIR}/cmake/CudaLayout.cmake"
      VERBATIM)
    add_custom_command(
      OUTPUT "${_gen}/${_shader}.fatbin"
      COMMAND "${CUDAToolkit_NVCC_EXECUTABLE}" -fatbin -O3 -std=c++17 ${_pen_nvcc_host} -gencode ${_pen_gencode}
              -o "${_gen}/${_shader}.fatbin" "${_gen}/${_shader}.cu"
      DEPENDS "${_gen}/${_shader}.cu"
      COMMENT "Compiling ${_shader} for sm_${_pen_sass} (nvcc)"
      VERBATIM)
    add_custom_command(
      OUTPUT "${_gen}/${_shader}_fatbin.cpp"
      COMMAND pen-embed-binary "${_gen}/${_shader}.fatbin" "${_gen}/${_shader}_fatbin.cpp" ${_name}
      DEPENDS "${_gen}/${_shader}.fatbin" pen-embed-binary
      VERBATIM)
    target_sources(penumbra_backends PRIVATE "${_gen}/${_shader}_fatbin.cpp" "${_gen}/${_layout}_cuda_layout.h")
  endforeach()

  target_sources(penumbra_backends PRIVATE cuda/cuda_backend.cpp gpu/gpu_bvh.cpp gpu/tinybvh.cpp)
  target_include_directories(penumbra_backends PRIVATE "${_gen}" ${CUDAToolkit_INCLUDE_DIRS})
  target_include_directories(penumbra_backends SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/tinybvh")
  target_compile_definitions(penumbra_backends PUBLIC PEN_HAS_CUDA=1)
  set_source_files_properties(cuda/cuda_backend.cpp PROPERTIES COMPILE_DEFINITIONS
                              "PEN_CUDA_SASS=${_pen_sass};PEN_CUDA_PTX=${_pen_ptx}")
  set_source_files_properties(gpu/tinybvh.cpp PROPERTIES COMPILE_OPTIONS "${_pen_no_warnings}")
  if(NOT WIN32)
    target_link_libraries(penumbra_backends PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()
