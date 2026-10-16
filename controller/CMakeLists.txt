# controller: scheduler, render controller, local worker; film writer, hosting and session file from M2/M4.
add_library(penumbra_controller STATIC local_worker.cpp render_controller.cpp scheduler.cpp solo_render.cpp)
target_link_libraries(penumbra_controller PUBLIC penumbra_worker)
