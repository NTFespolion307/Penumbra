# worker: device manager, device slots, test fault injection; client, scene cache and uploader from M4.
add_library(penumbra_worker STATIC device_manager.cpp device_slot.cpp test_faults.cpp)
target_link_libraries(penumbra_worker PUBLIC penumbra_backends)
