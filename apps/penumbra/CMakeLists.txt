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

# Regenerates the CLI reference in README.md from the same option table as --help.
add_custom_target(update-cli-docs
  COMMAND "${CMAKE_COMMAND}" -DEXE=$<TARGET_FILE:penumbra> -DREADME=${PROJECT_SOURCE_DIR}/README.md -DMODE=update
          -P "${PROJECT_SOURCE_DIR}/cmake/CliDocs.cmake"
  DEPENDS penumbra
  VERBATIM)
