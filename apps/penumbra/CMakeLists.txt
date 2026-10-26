# CLI definition is a library so the tests can parse command lines without launching the exe.
add_library(penumbra_cli STATIC cli/cli.cpp)
target_link_libraries(penumbra_cli PUBLIC penumbra_core PRIVATE CLI11::CLI11)
# GCC 12 false positive (bug 105329) in std::string code inlined from CLI11's add_option.
target_compile_options(penumbra_cli PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-restrict>)

# Console subsystem on Windows (so --terminal/--worker output works in cmd and PowerShell); the UI mode
# detaches the console when started from Explorer. See docs/GUIDE.md "Windows console gotcha".
add_executable(penumbra main.cpp platform.cpp progress.cpp modes/info.cpp)
if(WIN32)
  target_sources(penumbra PRIVATE penumbra.manifest)
endif()
target_link_libraries(penumbra PRIVATE penumbra_cli penumbra_controller penumbra_net)
set_target_properties(penumbra PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---- UI (M10, docs/GUIDE.md "UI"; DECISIONS D-100) ----
# Dear ImGui (third_party/imgui, v1.92.9) and GLFW (third_party/glfw, 3.4) are vendored: the vcpkg imgui port has
# no GLFW/Vulkan/OpenGL backends, and the vcpkg glfw3 port builds the whole X11 client library chain (autotools
# included) on Linux. GLFW needs only the X11 headers to build and opens libX11 at run time, and ImGui's OpenGL
# loader opens libGL at run time, so --terminal and --worker never need display libraries. X11 only: Wayland
# desktops run it through XWayland.
if(PEN_ENABLE_UI)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
  add_subdirectory("${PROJECT_SOURCE_DIR}/third_party/glfw" "${CMAKE_BINARY_DIR}/third_party/glfw" EXCLUDE_FROM_ALL)
  target_compile_options(glfw PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>)  # MSVC: GLFW sets its own /W3
  set(_imgui "${PROJECT_SOURCE_DIR}/third_party/imgui")
  add_library(penumbra_imgui STATIC
    "${_imgui}/imgui.cpp" "${_imgui}/imgui_draw.cpp" "${_imgui}/imgui_tables.cpp" "${_imgui}/imgui_widgets.cpp"
    "${_imgui}/backends/imgui_impl_glfw.cpp" "${_imgui}/backends/imgui_impl_opengl3.cpp")
  target_include_directories(penumbra_imgui SYSTEM PUBLIC "${_imgui}" "${_imgui}/backends")
  target_link_libraries(penumbra_imgui PUBLIC glfw)
  # No system OpenGL headers anywhere: ImGui's OpenGL backend has its own loader (Linux needs no libgl-dev).
  target_compile_definitions(penumbra_imgui PUBLIC GLFW_INCLUDE_NONE)
  # Vendored code: none of the project's warning flags (they would only clash with /W0).
  set_target_properties(penumbra_imgui PROPERTIES COMPILE_OPTIONS "")
  target_compile_options(penumbra_imgui PRIVATE "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0;/utf-8;/EHsc,-w>")
  if(PEN_ENABLE_VULKAN)
    find_package(volk CONFIG REQUIRED)
    target_sources(penumbra_imgui PRIVATE "${_imgui}/backends/imgui_impl_vulkan.cpp")
    target_compile_definitions(penumbra_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
    target_link_libraries(penumbra_imgui PUBLIC volk::volk volk::volk_headers)
  endif()

  # IBM Plex (SIL OFL 1.1, third_party/fonts) compiled into the binary so the UI looks the same everywhere.
  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
  set(_font_sources "")
  foreach(_font PlexSansRegular:IBMPlexSans-Regular PlexSansMedium:IBMPlexSans-Medium PlexSansSemiBold:IBMPlexSans-SemiBold
                PlexMonoRegular:IBMPlexMono-Regular PlexMonoMedium:IBMPlexMono-Medium)
    string(REPLACE ":" ";" _pair "${_font}")
    list(GET _pair 0 _name)
    list(GET _pair 1 _file)
    add_custom_command(
      OUTPUT "${_gen}/font_${_name}.cpp"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gen}"
      COMMAND pen-embed-binary "${PROJECT_SOURCE_DIR}/third_party/fonts/${_file}.ttf" "${_gen}/font_${_name}.cpp" k${_name}
      DEPENDS "${PROJECT_SOURCE_DIR}/third_party/fonts/${_file}.ttf" pen-embed-binary
      VERBATIM)
    list(APPEND _font_sources "${_gen}/font_${_name}.cpp")
  endforeach()

  add_library(penumbra_ui STATIC ui/app.cpp ui/file_dialog.cpp ui/model.cpp ui/page_dashboard.cpp ui/page_hosts.cpp
                                ui/page_render.cpp ui/page_settings.cpp ui/page_viewport.cpp ui/render_runner.cpp ui/scene_info.cpp ui/texture.cpp ui/theme.cpp
                                ui/widgets.cpp ui/gfx_gl.cpp ${_font_sources})
  if(WIN32)
    target_link_libraries(penumbra_ui PRIVATE ole32 shell32 uuid)  # the system file dialogs (COM)
  endif()
  if(PEN_ENABLE_VULKAN)
    target_sources(penumbra_ui PRIVATE ui/gfx_vulkan.cpp)
    target_compile_definitions(penumbra_ui PRIVATE PEN_UI_VULKAN=1)
  endif()
  target_include_directories(penumbra_ui SYSTEM PRIVATE ${STB_INCLUDE_DIRS})
  target_link_libraries(penumbra_ui PUBLIC penumbra_core PRIVATE penumbra_imgui penumbra_controller)
  target_link_libraries(penumbra PRIVATE penumbra_ui)
  target_compile_definitions(penumbra PRIVATE PEN_HAS_UI=1)
endif()

# Regenerates the CLI reference in README.md from the same option table as --help.
add_custom_target(update-cli-docs
  COMMAND "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:penumbra> -DREADME=${PROJECT_SOURCE_DIR}/README.md -DMODE=update
          -P "${PROJECT_SOURCE_DIR}/cmake/CliDocs.cmake"
  DEPENDS penumbra
  VERBATIM)
