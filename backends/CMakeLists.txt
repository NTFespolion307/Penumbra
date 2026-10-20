# backends: IBackend implementations: cpu/ (Embree, M1), null/ (test only), vulkan/ (M5), cuda/ (M6), optix/ (M13).
# The only place that may include Vulkan, CUDA, OptiX or Embree headers.
add_library(penumbra_backends STATIC cpu/cpu_backend.cpp null/null_backend.cpp)
target_link_libraries(penumbra_backends PUBLIC penumbra_core PRIVATE embree TBB::tbb)

if(PEN_ENABLE_VULKAN)
  # The shaders (shaders/*.slang) compile to one SPIR-V module at build time with the pinned slangc from vcpkg
  # (a host tool), and the module is embedded in the binary (DECISIONS D-076).
  find_package(volk CONFIG REQUIRED)
  find_package(VulkanMemoryAllocator CONFIG REQUIRED)
  file(GLOB _pen_slangc_dirs "${VCPKG_INSTALLED_DIR}/*/tools/shader-slang")
  find_program(PEN_SLANGC slangc REQUIRED HINTS ${_pen_slangc_dirs} NO_DEFAULT_PATH)
  file(GLOB PEN_SHADER_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/shaders/*.slang")
  set(_spv "${CMAKE_CURRENT_BINARY_DIR}/generated/pathtrace.spv")
  set(_spv_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/pathtrace_spv.cpp")
  add_custom_command(
    OUTPUT "${_spv}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND "${PEN_SLANGC}" "${PROJECT_SOURCE_DIR}/shaders/pathtrace.slang" -target spirv -profile spirv_1_5
            -fvk-use-scalar-layout -fvk-use-entrypoint-name -O3 -o "${_spv}"
    DEPENDS ${PEN_SHADER_SOURCES}
    COMMENT "Compiling the Vulkan path tracer (Slang -> SPIR-V)"
    VERBATIM)
  add_custom_command(
    OUTPUT "${_spv_cpp}"
    COMMAND "${CMAKE_COMMAND}" -DINPUT=${_spv} -DOUTPUT=${_spv_cpp} -DNAME=kPathtraceSpv
            -P "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    DEPENDS "${_spv}" "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
    VERBATIM)
  target_sources(penumbra_backends PRIVATE vulkan/vulkan_backend.cpp vulkan/vma.cpp "${_spv_cpp}")
  target_link_libraries(penumbra_backends PRIVATE volk::volk volk::volk_headers GPUOpen::VulkanMemoryAllocator)
  target_compile_definitions(penumbra_backends PUBLIC PEN_HAS_VULKAN=1)
  # VMA's implementation trips compiler warnings that are not ours.
  set_source_files_properties(vulkan/vma.cpp PROPERTIES COMPILE_OPTIONS
                              "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")
endif()
