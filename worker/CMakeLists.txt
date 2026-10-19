# worker: device manager, device slots, test fault injection, network client; scene cache and uploader from M4.
add_library(penumbra_worker STATIC device_manager.cpp device_prep.cpp device_slot.cpp scene_cache.cpp test_faults.cpp
                                   client/worker_client.cpp)
target_link_libraries(penumbra_worker PUBLIC penumbra_backends PRIVATE penumbra_net)
