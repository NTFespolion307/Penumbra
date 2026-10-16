# net: FlatBuffers schemas (schema/*.fbs), framing, TLS-PSK channel, chunked transfer. Built from M4.
add_library(penumbra_net INTERFACE)
target_link_libraries(penumbra_net INTERFACE penumbra_core)
