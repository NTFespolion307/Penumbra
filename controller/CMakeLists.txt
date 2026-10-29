# controller: scheduler, render controller, local worker, film writer and session file; hosting (M4).
add_library(penumbra_controller STATIC benchmark.cpp local_worker.cpp render_controller.cpp render_job.cpp scheduler.cpp solo_render.cpp
                                       hosting/host.cpp hosting/hosting_session.cpp hosting/remote_worker.cpp hosting/scene_uploader.cpp
                                       hosting/upnp.cpp)
find_package(miniupnpc CONFIG REQUIRED)
target_link_libraries(penumbra_controller PUBLIC penumbra_worker PRIVATE penumbra_net miniupnpc::miniupnpc)
