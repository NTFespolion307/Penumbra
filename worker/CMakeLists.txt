# worker: device manager, device slots; client, scene cache and uploader from M4.
add_library(penumbra_worker STATIC device_manager.cpp device_slot.cpp)
target_link_libraries(penumbra_worker PUBLIC penumbra_backends)
