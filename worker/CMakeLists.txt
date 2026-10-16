# worker: client, scene cache, device manager, uploader. Built from M3.
add_library(penumbra_worker INTERFACE)
target_link_libraries(penumbra_worker INTERFACE penumbra_core)
