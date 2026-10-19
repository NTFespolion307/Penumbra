# net: FlatBuffers schemas (schema/*.fbs), framing, TLS 1.3 with join-code keys, connections, tile encoding.
find_package(asio CONFIG REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(MbedTLS CONFIG REQUIRED)

# flatc comes from the vcpkg host tools (an imported target when the host triplet differs from the target).
if(TARGET flatbuffers::flatc)
  set(PEN_FLATC flatbuffers::flatc)
else()
  file(GLOB _pen_flatc_dirs "${VCPKG_INSTALLED_DIR}/*/tools/flatbuffers")
  find_program(PEN_FLATC flatc REQUIRED HINTS ${_pen_flatc_dirs} NO_DEFAULT_PATH)
endif()

set(PEN_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/schema/penumbra.fbs")
set(PEN_SCHEMA_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
  OUTPUT "${PEN_SCHEMA_OUT}/penumbra_generated.h"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${PEN_SCHEMA_OUT}"
  COMMAND ${PEN_FLATC} --cpp --gen-object-api --cpp-std c++17 -o "${PEN_SCHEMA_OUT}" "${PEN_SCHEMA}"
  DEPENDS "${PEN_SCHEMA}" ${PEN_FLATC}
  COMMENT "Generating wire protocol code from penumbra.fbs"
  VERBATIM)
add_custom_target(penumbra_schema DEPENDS "${PEN_SCHEMA_OUT}/penumbra_generated.h")

add_library(penumbra_net STATIC connection.cpp frame.cpp join_code.cpp tile_codec.cpp tls.cpp)
add_dependencies(penumbra_net penumbra_schema)
target_include_directories(penumbra_net PUBLIC "${PEN_SCHEMA_OUT}")
target_compile_definitions(penumbra_net PUBLIC ASIO_STANDALONE ASIO_NO_DEPRECATED)
if(WIN32)
  target_compile_definitions(penumbra_net PUBLIC _WIN32_WINNT=0x0A00)  # Windows 10
  target_link_libraries(penumbra_net PUBLIC ws2_32 mswsock)
endif()
target_link_libraries(penumbra_net
  PUBLIC penumbra_core asio::asio flatbuffers::flatbuffers
  PRIVATE zstd::libzstd MbedTLS::mbedtls)
