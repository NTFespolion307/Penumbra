# backends: IBackend implementations: cpu/ (Embree, M1), vulkan/ (M5), cuda/ (M6), optix/ (M13).
# The only place that may include Vulkan, CUDA, OptiX or Embree headers.
add_library(penumbra_backends STATIC cpu/cpu_backend.cpp)
target_link_libraries(penumbra_backends PUBLIC penumbra_core PRIVATE embree TBB::tbb)
