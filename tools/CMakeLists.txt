# tools: penumbra-diff (image comparison), penumbra-scenegen (test scenes); netem-proxy from M4.

add_executable(penumbra-diff penumbra_diff.cpp)
target_link_libraries(penumbra-diff PRIVATE penumbra_core)

add_executable(penumbra-scenegen scenegen.cpp)
target_include_directories(penumbra-scenegen SYSTEM PRIVATE ${STB_INCLUDE_DIRS})
target_link_libraries(penumbra-scenegen PRIVATE penumbra_core)

set_target_properties(penumbra-diff penumbra-scenegen PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
