# tools: penumbra-diff (image comparison), penumbra-scenegen (test scenes), penumbra-netem-proxy (bad networks).

add_executable(penumbra-diff penumbra_diff.cpp)
target_link_libraries(penumbra-diff PRIVATE penumbra_core)

add_executable(penumbra-scenegen scenegen.cpp)
target_include_directories(penumbra-scenegen SYSTEM PRIVATE ${STB_INCLUDE_DIRS})
target_link_libraries(penumbra-scenegen PRIVATE penumbra_core)

add_executable(penumbra-netem-proxy netem_proxy.cpp)
target_link_libraries(penumbra-netem-proxy PRIVATE penumbra_net)  # Asio and its settings

set_target_properties(penumbra-diff penumbra-scenegen penumbra-netem-proxy PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
