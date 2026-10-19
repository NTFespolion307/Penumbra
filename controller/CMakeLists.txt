# controller: scheduler, render controller, local worker, film writer and session file; hosting (M4).
add_library(penumbra_controller STATIC local_worker.cpp render_controller.cpp scheduler.cpp solo_render.cpp
                                       hosting/host.cpp hosting/remote_worker.cpp hosting/scene_uploader.cpp)
target_link_libraries(penumbra_controller PUBLIC penumbra_worker PRIVATE penumbra_net)
