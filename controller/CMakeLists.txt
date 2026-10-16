# controller: session, scheduler (items, leases), film writer, local worker, hosting, session file. Built from M2.
add_library(penumbra_controller INTERFACE)
target_link_libraries(penumbra_controller INTERFACE penumbra_core)
