# Enforces the layering rule from CLAUDE.md:
#  - GPU API headers (Vulkan, CUDA, OptiX, Embree) only in backends/ (and apps/penumbra/ui/ for the swapchain);
#  - socket and TLS headers only in net/, controller/hosting/, worker/client/;
#  - core/ includes neither.
# Input: SOURCE_DIR.

set(gpu_regex "#[ \t]*include[ \t]*[<\"](vulkan/|volk|vk_mem_alloc|cuda|optix|embree|tinybvh|nvml)")
set(socket_regex "#[ \t]*include[ \t]*[<\"](asio|boost/asio|winsock|ws2tcpip|sys/socket|netinet/|arpa/inet|netdb|mbedtls/|miniupnpc)")

file(GLOB_RECURSE sources RELATIVE "${SOURCE_DIR}"
     "${SOURCE_DIR}/core/*" "${SOURCE_DIR}/controller/*" "${SOURCE_DIR}/worker/*" "${SOURCE_DIR}/net/*"
     "${SOURCE_DIR}/backends/*" "${SOURCE_DIR}/apps/*")
list(FILTER sources INCLUDE REGEX "\\.(h|hpp|c|cc|cpp|cu|cuh)$")

set(violations "")
foreach(f IN LISTS sources)
  file(STRINGS "${SOURCE_DIR}/${f}" lines REGEX "#[ \t]*include")
  foreach(line IN LISTS lines)
    if(line MATCHES "${gpu_regex}" AND NOT f MATCHES "^(backends/|apps/penumbra/ui/)")
      string(APPEND violations "  ${f}: GPU header outside backends/: ${line}\n")
    endif()
    if(line MATCHES "${socket_regex}" AND NOT f MATCHES "^(net/|controller/hosting/|worker/client/)")
      string(APPEND violations "  ${f}: socket header outside net/: ${line}\n")
    endif()
  endforeach()
endforeach()

if(violations)
  message(FATAL_ERROR "Layering violations:\n${violations}")
endif()
list(LENGTH sources n)
message(STATUS "layering OK (${n} files checked)")
