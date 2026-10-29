# tools: penumbra-diff (image comparison), penumbra-scenegen (test scenes), penumbra-netem-proxy (bad networks),
# penumbra-terrain (real terrain scenes).

add_executable(penumbra-diff penumbra_diff.cpp)
target_link_libraries(penumbra-diff PRIVATE penumbra_core)

add_executable(penumbra-scenegen scenegen.cpp)
target_include_directories(penumbra-scenegen SYSTEM PRIVATE ${STB_INCLUDE_DIRS})
target_link_libraries(penumbra-scenegen PRIVATE penumbra_core)

# M14: swisstopo elevation and photo tiles into a glTF terrain (scripts/get-scenes swiss-alps, D-122).
add_executable(penumbra-terrain terrain.cpp)
target_include_directories(penumbra-terrain SYSTEM PRIVATE ${STB_INCLUDE_DIRS})
target_link_libraries(penumbra-terrain PRIVATE TIFF::TIFF nlohmann_json::nlohmann_json)

add_executable(penumbra-netem-proxy netem_proxy.cpp)
target_link_libraries(penumbra-netem-proxy PRIVATE penumbra_net)  # Asio and its settings

# Build tool: a binary file as a C++ byte array (CUDA kernels, UI fonts).
add_executable(pen-embed-binary embed_binary.cpp)

set_target_properties(penumbra-diff penumbra-scenegen penumbra-netem-proxy penumbra-terrain PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
