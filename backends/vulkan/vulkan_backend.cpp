#include "backends/vulkan/vulkan_backend.h"

#include <volk.h>
#include <vk_mem_alloc.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fmt/format.h>
#include <functional>
#include <mutex>

#include "backends/gpu/gpu_scene.h"
#include "core/shading/bsdf.h"
#include "core/settings/parse.h"

namespace pen {

// SPIR-V of shaders/pathtrace.slang, generated at build time.
extern const uint32_t* const kPathtraceSpv;
extern const size_t kPathtraceSpvWords;
extern const uint32_t* const kParitySpv;
extern const size_t kParitySpvWords;

namespace {

const char* const kDeviceExtensions[] = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
};

std::string vkResultText(VkResult r) {
  switch (r) {
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "out of host memory";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "out of GPU memory";
    case VK_ERROR_DEVICE_LOST: return "device lost (driver reset or GPU removed)";
    case VK_ERROR_INITIALIZATION_FAILED: return "initialization failed";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "extension not present";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "feature not present";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "incompatible driver";
    default: return fmt::format("VkResult {}", static_cast<int>(r));
  }
}

void check(VkResult r, const char* what) {
  if (r != VK_SUCCESS) throw BackendError(fmt::format("Vulkan: {} failed: {}", what, vkResultText(r)));
}

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return a ? (v + a - 1) / a * a : v; }

// ---------------------------------------------------------------- instance and probing

struct InstanceState {
  VulkanProbe probe;
  VkInstance instance = VK_NULL_HANDLE;
  std::vector<VkPhysicalDevice> physical;
};

InstanceState& instanceState() {
  static InstanceState state = [] {
    InstanceState s;
    if (volkInitialize() != VK_SUCCESS) {
      s.probe.error = "no Vulkan driver found (vulkan-1.dll / libvulkan.so.1)";
      return s;
    }
    s.probe.loader_found = true;
    const uint32_t loaderVersion = volkGetInstanceVersion();
    if (loaderVersion < VK_API_VERSION_1_2) {
      s.probe.error = "the Vulkan loader is older than 1.2";
      return s;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Penumbra";
    app.pEngineName = "Penumbra";
    app.apiVersion = loaderVersion >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    if (vkCreateInstance(&ci, nullptr, &s.instance) != VK_SUCCESS) {
      s.probe.error = "cannot create a Vulkan instance";
      return s;
    }
    volkLoadInstanceOnly(s.instance);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(s.instance, &n, nullptr);
    s.physical.resize(n);
    vkEnumeratePhysicalDevices(s.instance, &n, s.physical.data());

    for (uint32_t i = 0; i < n; ++i) {
      VkPhysicalDevice pd = s.physical[i];
      VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
      VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      driver.pNext = &ids;
      VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      props.pNext = &driver;
      vkGetPhysicalDeviceProperties2(pd, &props);
      VulkanGpu g;
      g.physical_index = i;
      g.name = props.properties.deviceName;
      std::memcpy(g.uuid.data(), ids.deviceUUID, VK_UUID_SIZE);
      g.driver = fmt::format("{} {}", driver.driverName, driver.driverInfo);
      switch (props.properties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: g.type = "discrete"; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: g.type = "integrated"; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: g.type = "virtual"; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: g.type = "software"; break;
        default: g.type = "other"; break;
      }
      VkPhysicalDeviceMemoryProperties mem;
      vkGetPhysicalDeviceMemoryProperties(pd, &mem);
      for (uint32_t h = 0; h < mem.memoryHeapCount; ++h)
        if (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
          g.vram_bytes = std::max<uint64_t>(g.vram_bytes, mem.memoryHeaps[h].size);

      std::vector<std::string> missing;
      if (props.properties.apiVersion < VK_API_VERSION_1_2) missing.push_back("Vulkan 1.2");
      uint32_t extCount = 0;
      vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, nullptr);
      std::vector<VkExtensionProperties> exts(extCount);
      vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, exts.data());
      for (const char* want : kDeviceExtensions) {
        const bool has = std::any_of(exts.begin(), exts.end(),
                                     [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, want) == 0; });
        if (!has) missing.push_back(want);
      }
      if (missing.empty()) {
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        rt.pNext = &as;
        as.pNext = &v12;
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f.pNext = &rt;
        vkGetPhysicalDeviceFeatures2(pd, &f);
        if (!rt.rayTracingPipeline) missing.push_back("rayTracingPipeline");
        if (!as.accelerationStructure) missing.push_back("accelerationStructure");
        if (!v12.bufferDeviceAddress) missing.push_back("bufferDeviceAddress");
        if (!v12.scalarBlockLayout) missing.push_back("scalarBlockLayout");
      }
      g.ray_tracing = missing.empty();
      for (const auto& m : missing) g.missing += (g.missing.empty() ? "" : ", ") + m;
      s.probe.gpus.push_back(g);
    }
    return s;
  }();
  return state;
}

// ---------------------------------------------------------------- buffers

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation alloc = nullptr;
  VkDeviceSize size = 0;
  VkDeviceAddress address = 0;
  void* mapped = nullptr;
};

class VulkanBackend final : public IBackend {
 public:
  explicit VulkanBackend(uint32_t physicalIndex);
  ~VulkanBackend() override;

  DeviceInfo info() const override { return info_; }
  MemoryPlan plan(const SceneStats& stats) override;
  void upload(const Scene& scene, const MemoryPlan& plan) override;
  void prepare(const FrameSetup& frame) override;
  void render(const WorkItem& item, TileResult& out) override;
  void cancel() override { cancel_ = true; }
  std::vector<float> runParity(uint32_t count);

 private:
  enum class Memory { Device, Upload, Readback };

  Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Memory memory);
  void destroy(Buffer& b);
  // A device buffer filled with `bytes` (at least 16 bytes, so empty arrays still bind).
  Buffer deviceBuffer(const void* data, size_t bytes, VkBufferUsageFlags usage);
  template <class T>
  Buffer deviceBuffer(const std::vector<T>& v, VkBufferUsageFlags usage = 0) {
    return deviceBuffer(v.data(), v.size() * sizeof(T), usage);
  }
  void submitAndWait(const std::function<void(VkCommandBuffer)>& record);
  void releaseScene();
  void buildAccelerationStructures(const Scene& scene);
  void createPipeline();
  void writeDescriptors();
  void ensureOutput(uint64_t pixels);

  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VolkDeviceTable vk_{};
  VmaAllocator vma_ = nullptr;
  uint32_t queue_family_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt_props_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
  VkPhysicalDeviceAccelerationStructurePropertiesKHR as_props_{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
  DeviceInfo info_;
  uint64_t vram_budget_ = 0;

  // Scene
  std::vector<Buffer> scene_buffers_;  // indexed by binding (some unused)
  std::vector<Buffer> blas_buffers_;
  std::vector<VkAccelerationStructureKHR> blas_;
  Buffer tlas_buffer_, instance_buffer_;
  VkAccelerationStructureKHR tlas_ = VK_NULL_HANDLE;
  bool scene_ready_ = false;
  const Scene* scene_ = nullptr;  // owned by the caller while the backend renders it

  // Frame and item
  Buffer frame_buffer_, out_buffer_, counter_buffer_;
  uint64_t out_capacity_ = 0;

  // Pipeline
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet set_ = VK_NULL_HANDLE;
  Buffer sbt_;
  VkStridedDeviceAddressRegionKHR rgen_{}, miss_{}, hit_{}, call_{};

  std::atomic<bool> cancel_{false};
  bool lost_ = false;  // the GPU stopped answering: nothing is waited for or destroyed any more (D-094)
};

// A cancelled GPU job that has not finished after this long means the GPU is hung (D-094).
constexpr int kHungGraceSeconds = 10;

// Descriptor bindings (shaders/*.slang).
constexpr uint32_t kBindTlas = 0, kBindFrame = 1, kBindOut = 23, kBindCounters = 24, kBindCount = 25;

VulkanBackend::VulkanBackend(uint32_t physicalIndex) {
  InstanceState& st = instanceState();
  if (physicalIndex >= st.physical.size()) throw BackendError(fmt::format("there is no Vulkan GPU {}", physicalIndex));
  const VulkanGpu& gpu = st.probe.gpus[physicalIndex];
  if (!gpu.ray_tracing) throw BackendError(fmt::format("{} has no Vulkan ray tracing ({})", gpu.name, gpu.missing));
  physical_ = st.physical[physicalIndex];

  rt_props_.pNext = &as_props_;
  VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  props.pNext = &rt_props_;
  vkGetPhysicalDeviceProperties2(physical_, &props);

  // A queue that can trace rays (compute).
  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, qf.data());
  bool found = false;
  for (uint32_t i = 0; i < qn && !found; ++i)
    if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      queue_family_ = i;
      found = true;
    }
  if (!found) throw BackendError(fmt::format("{} has no compute queue", gpu.name));

  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
  rt.rayTracingPipeline = VK_TRUE;
  VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  as.accelerationStructure = VK_TRUE;
  VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  v12.bufferDeviceAddress = VK_TRUE;
  v12.scalarBlockLayout = VK_TRUE;
  rt.pNext = &as;
  as.pNext = &v12;
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features.pNext = &rt;

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queue_family_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &features;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = static_cast<uint32_t>(std::size(kDeviceExtensions));
  dci.ppEnabledExtensionNames = kDeviceExtensions;
  check(vkCreateDevice(physical_, &dci, nullptr, &device_), "vkCreateDevice");
  volkLoadDeviceTable(&vk_, device_);
  vk_.vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

  VmaVulkanFunctions fns{};
  fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
  VmaAllocatorCreateInfo aci{};
  aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  aci.physicalDevice = physical_;
  aci.device = device_;
  aci.instance = st.instance;
  aci.vulkanApiVersion = VK_API_VERSION_1_2;
  aci.pVulkanFunctions = &fns;
  check(vmaCreateAllocator(&aci, &vma_), "vmaCreateAllocator");

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = queue_family_;
  check(vk_.vkCreateCommandPool(device_, &pci, nullptr, &pool_), "vkCreateCommandPool");
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool_;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  check(vk_.vkAllocateCommandBuffers(device_, &cai, &cmd_), "vkAllocateCommandBuffers");
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  check(vk_.vkCreateFence(device_, &fci, nullptr, &fence_), "vkCreateFence");

  info_.name = gpu.name;
  info_.backend = BackendKind::Vulkan;
  info_.threads = 0;
  info_.memory_bytes = gpu.vram_bytes;
  info_.hardware_rt = true;
  info_.uuid = gpu.uuid;
  vram_budget_ = gpu.vram_bytes / 10 * 9;  // docs/GUIDE.md "Budget": free VRAM minus 10% (M8 reads the live budget)

  frame_buffer_ = createBuffer(sizeof(gpu::Frame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, Memory::Upload);
  counter_buffer_ = createBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Memory::Readback);
  createPipeline();
}

VulkanBackend::~VulkanBackend() {
  if (lost_) return;  // its work may still be running: leak it rather than hang or free memory in use
  if (device_) vk_.vkDeviceWaitIdle(device_);
  releaseScene();
  destroy(frame_buffer_);
  destroy(out_buffer_);
  destroy(counter_buffer_);
  destroy(sbt_);
  if (pipeline_) vk_.vkDestroyPipeline(device_, pipeline_, nullptr);
  if (pipeline_layout_) vk_.vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
  if (descriptor_pool_) vk_.vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
  if (set_layout_) vk_.vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
  if (fence_) vk_.vkDestroyFence(device_, fence_, nullptr);
  if (pool_) vk_.vkDestroyCommandPool(device_, pool_, nullptr);
  if (vma_) vmaDestroyAllocator(vma_);
  if (device_) vk_.vkDestroyDevice(device_, nullptr);
}

Buffer VulkanBackend::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Memory memory) {
  Buffer b;
  b.size = std::max<VkDeviceSize>(size, 16);
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = b.size;
  bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_AUTO;
  if (memory == Memory::Upload)
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  if (memory == Memory::Readback)
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VmaAllocationInfo ai{};
  const VkResult r = vmaCreateBuffer(vma_, &bci, &aci, &b.buffer, &b.alloc, &ai);
  if (r != VK_SUCCESS)
    throw BackendError(fmt::format("Vulkan: cannot allocate {} on {}: {}", formatBytes(b.size), info_.name, vkResultText(r)));
  b.mapped = ai.pMappedData;
  VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  dai.buffer = b.buffer;
  b.address = vk_.vkGetBufferDeviceAddress(device_, &dai);
  return b;
}

void VulkanBackend::destroy(Buffer& b) {
  if (b.buffer) vmaDestroyBuffer(vma_, b.buffer, b.alloc);
  b = Buffer{};
}

void VulkanBackend::submitAndWait(const std::function<void(VkCommandBuffer)>& record) {
  check(vk_.vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer");
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  check(vk_.vkBeginCommandBuffer(cmd_, &bi), "vkBeginCommandBuffer");
  record(cmd_);
  check(vk_.vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  check(vk_.vkResetFences(device_, 1, &fence_), "vkResetFences");
  check(vk_.vkQueueSubmit(queue_, 1, &si, fence_), "vkQueueSubmit");
  // Waits in slices, so a GPU that stops answering cannot hold its slot (and the end of the render) forever: once
  // cancelled, the job gets kHungGraceSeconds to finish, then the device counts as lost (DECISIONS D-094).
  std::chrono::steady_clock::time_point cancelledAt{};
  for (;;) {
    const VkResult r = vk_.vkWaitForFences(device_, 1, &fence_, VK_TRUE, 100'000'000);
    if (r == VK_SUCCESS) return;
    if (r != VK_TIMEOUT) check(r, "vkWaitForFences");
    if (!cancel_) continue;
    const auto now = std::chrono::steady_clock::now();
    if (cancelledAt == std::chrono::steady_clock::time_point{}) cancelledAt = now;
    if (now - cancelledAt > std::chrono::seconds(kHungGraceSeconds)) {
      lost_ = true;
      throw BackendError(fmt::format("Vulkan: {} does not answer: its job still runs {} s after the cancel",
                                     info_.name, kHungGraceSeconds));
    }
  }
}

Buffer VulkanBackend::deviceBuffer(const void* data, size_t bytes, VkBufferUsageFlags usage) {
  Buffer dst = createBuffer(bytes, usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            Memory::Device);
  if (bytes == 0) return dst;
  Buffer staging = createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Memory::Upload);
  std::memcpy(staging.mapped, data, bytes);
  vmaFlushAllocation(vma_, staging.alloc, 0, VK_WHOLE_SIZE);
  submitAndWait([&](VkCommandBuffer c) {
    VkBufferCopy region{0, 0, bytes};
    vk_.vkCmdCopyBuffer(c, staging.buffer, dst.buffer, 1, &region);
  });
  destroy(staging);
  return dst;
}

// ---------------------------------------------------------------- memory plan and scene

MemoryPlan VulkanBackend::plan(const SceneStats& stats) {
  // Geometry is uploaded once as attributes and once inside the acceleration structures (about 2x).
  const uint64_t need = stats.geometry_bytes * 3 + stats.texture_bytes;
  if (need > vram_budget_)
    throw BackendError(fmt::format("the scene needs about {} of GPU memory, {} has {} usable; memory tiers (textures "
                                   "in RAM) arrive in M8",
                                   formatBytes(need), info_.name, formatBytes(vram_budget_)));
  MemoryPlan p;
  p.tier = 0;
  p.summary = fmt::format("tier 0, scene about {} of {} in VRAM", formatBytes(need), formatBytes(vram_budget_));
  return p;
}

void VulkanBackend::releaseScene() {
  for (auto& b : scene_buffers_) destroy(b);
  scene_buffers_.clear();
  for (auto as : blas_) vk_.vkDestroyAccelerationStructureKHR(device_, as, nullptr);
  blas_.clear();
  for (auto& b : blas_buffers_) destroy(b);
  blas_buffers_.clear();
  if (tlas_) vk_.vkDestroyAccelerationStructureKHR(device_, tlas_, nullptr);
  tlas_ = VK_NULL_HANDLE;
  destroy(tlas_buffer_);
  destroy(instance_buffer_);
  scene_ready_ = false;
}

void VulkanBackend::upload(const Scene& scene, const MemoryPlan&) {
  const auto t0 = std::chrono::steady_clock::now();
  vk_.vkDeviceWaitIdle(device_);
  releaseScene();

  const GpuScene g = flattenScene(scene);

  constexpr VkBufferUsageFlags kAsInput = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  scene_buffers_.resize(kBindCount);
  scene_buffers_[2] = deviceBuffer(g.instances);
  scene_buffers_[3] = deviceBuffer(g.primitives);
  scene_buffers_[4] = deviceBuffer(g.positions, kAsInput);
  scene_buffers_[5] = deviceBuffer(g.normals);
  scene_buffers_[6] = deviceBuffer(g.tangents);
  scene_buffers_[7] = deviceBuffer(g.uvs);
  scene_buffers_[8] = deviceBuffer(g.colors);
  scene_buffers_[9] = deviceBuffer(g.indices, kAsInput);
  scene_buffers_[10] = deviceBuffer(g.materials);
  scene_buffers_[11] = deviceBuffer(g.textures);
  scene_buffers_[12] = deviceBuffer(g.texels8);
  scene_buffers_[13] = deviceBuffer(g.texels32);
  scene_buffers_[14] = deviceBuffer(g.emissive);
  scene_buffers_[15] = deviceBuffer(g.emissive_offset);
  scene_buffers_[16] = deviceBuffer(g.light_table);
  scene_buffers_[17] = deviceBuffer(g.lights);
  scene_buffers_[18] = deviceBuffer(*g.env_map);
  scene_buffers_[19] = deviceBuffer(g.env_tables);
  scene_buffers_[20] = deviceBuffer(g.sheen_table);
  scene_buffers_[21] = deviceBuffer(g.srgb_table);
  scene_buffers_[22] = deviceBuffer(*g.env_weights);

  buildAccelerationStructures(scene);
  scene_ = &scene;
  scene_ready_ = true;
  spdlog::debug("{}: scene uploaded and acceleration structures built in {:.2f}s", info_.name,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

void VulkanBackend::buildAccelerationStructures(const Scene& scene) {
  const VkDeviceAddress posAddr = scene_buffers_[4].address, idxAddr = scene_buffers_[9].address;
  const VkDeviceSize scratchAlign = as_props_.minAccelerationStructureScratchOffsetAlignment;

  // ---- One BLAS per mesh; geometry index = primitive index ----
  std::vector<VkDeviceAddress> blasAddress(scene.meshes.size(), 0);
  blas_.assign(scene.meshes.size(), VK_NULL_HANDLE);
  blas_buffers_.resize(scene.meshes.size());
  uint32_t vertexBase = 0, indexBase = 0;
  for (size_t m = 0; m < scene.meshes.size(); ++m) {
    const Mesh& mesh = scene.meshes[m];
    std::vector<VkAccelerationStructureGeometryKHR> geoms;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<uint32_t> counts;
    uint32_t total = 0;
    for (const MeshPrimitive& p : mesh.primitives) {
      VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
      g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
      g.flags = scene.materials[p.material].usesAlpha() ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
      auto& tri = g.geometry.triangles;
      tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
      tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
      tri.vertexData.deviceAddress = posAddr + VkDeviceAddress(vertexBase) * sizeof(Vec3f);
      tri.vertexStride = sizeof(Vec3f);
      tri.maxVertex = p.positions.empty() ? 0 : static_cast<uint32_t>(p.positions.size() - 1);
      tri.indexType = VK_INDEX_TYPE_UINT32;
      tri.indexData.deviceAddress = idxAddr + VkDeviceAddress(indexBase) * sizeof(uint32_t);
      geoms.push_back(g);
      ranges.push_back({p.triangleCount(), 0, 0, 0});
      counts.push_back(p.triangleCount());
      total += p.triangleCount();
      vertexBase += static_cast<uint32_t>(p.positions.size());
      indexBase += static_cast<uint32_t>(p.indices.size());
    }
    if (total == 0) continue;  // nothing to hit: its instances are left out of the TLAS

    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = static_cast<uint32_t>(geoms.size());
    build.pGeometries = geoms.data();
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vk_.vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                                                counts.data(), &sizes);
    blas_buffers_[m] = createBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                                    Memory::Device);
    VkAccelerationStructureCreateInfoKHR asci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    asci.buffer = blas_buffers_[m].buffer;
    asci.size = sizes.accelerationStructureSize;
    asci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    check(vk_.vkCreateAccelerationStructureKHR(device_, &asci, nullptr, &blas_[m]), "vkCreateAccelerationStructureKHR");
    Buffer scratch = createBuffer(sizes.buildScratchSize + scratchAlign, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Memory::Device);
    build.dstAccelerationStructure = blas_[m];
    build.scratchData.deviceAddress = alignUp(scratch.address, scratchAlign);
    const VkAccelerationStructureBuildRangeInfoKHR* pRanges = ranges.data();
    submitAndWait([&](VkCommandBuffer c) { vk_.vkCmdBuildAccelerationStructuresKHR(c, 1, &build, &pRanges); });
    destroy(scratch);
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = blas_[m];
    blasAddress[m] = vk_.vkGetAccelerationStructureDeviceAddressKHR(device_, &ai);
  }

  // ---- TLAS: one instance per scene instance with geometry; custom index = scene instance ----
  std::vector<VkAccelerationStructureInstanceKHR> inst;
  for (uint32_t i = 0; i < scene.instances.size(); ++i) {
    const Instance& s = scene.instances[i];
    if (!blasAddress[s.mesh]) continue;
    VkAccelerationStructureInstanceKHR v{};
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c) v.transform.matrix[r][c] = s.to_world.m[r][c];
    v.instanceCustomIndex = i;
    v.mask = 0xff;
    v.instanceShaderBindingTableRecordOffset = 0;
    v.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;  // Embree does not cull either
    v.accelerationStructureReference = blasAddress[s.mesh];
    inst.push_back(v);
  }
  instance_buffer_ = deviceBuffer(inst, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);

  VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  g.geometry.instances.data.deviceAddress = instance_buffer_.address;
  VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &g;
  const uint32_t count = static_cast<uint32_t>(inst.size());
  VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vk_.vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &count,
                                              &sizes);
  tlas_buffer_ = createBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                              Memory::Device);
  VkAccelerationStructureCreateInfoKHR asci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  asci.buffer = tlas_buffer_.buffer;
  asci.size = sizes.accelerationStructureSize;
  asci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  check(vk_.vkCreateAccelerationStructureKHR(device_, &asci, nullptr, &tlas_), "vkCreateAccelerationStructureKHR");
  Buffer scratch = createBuffer(sizes.buildScratchSize + scratchAlign, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Memory::Device);
  build.dstAccelerationStructure = tlas_;
  build.scratchData.deviceAddress = alignUp(scratch.address, scratchAlign);
  VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
  const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
  submitAndWait([&](VkCommandBuffer c) { vk_.vkCmdBuildAccelerationStructuresKHR(c, 1, &build, &pRange); });
  destroy(scratch);
}

// ---------------------------------------------------------------- pipeline

void VulkanBackend::createPipeline() {
  std::vector<VkDescriptorSetLayoutBinding> b;
  for (uint32_t i = 0; i < kBindCount; ++i) {
    VkDescriptorSetLayoutBinding x{};
    x.binding = i;
    x.descriptorCount = 1;
    x.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                   VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
    x.descriptorType = i == kBindTlas    ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                       : i == kBindFrame ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                         : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b.push_back(x);
  }
  VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.bindingCount = static_cast<uint32_t>(b.size());
  lci.pBindings = b.data();
  check(vk_.vkCreateDescriptorSetLayout(device_, &lci, nullptr, &set_layout_), "vkCreateDescriptorSetLayout");

  VkPushConstantRange pcr{VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(gpu::PushConstants)};
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &set_layout_;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  check(vk_.vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_), "vkCreatePipelineLayout");

  VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBindCount}};
  VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 1;
  dpci.poolSizeCount = 3;
  dpci.pPoolSizes = sizes;
  check(vk_.vkCreateDescriptorPool(device_, &dpci, nullptr, &descriptor_pool_), "vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = descriptor_pool_;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &set_layout_;
  check(vk_.vkAllocateDescriptorSets(device_, &dsai, &set_), "vkAllocateDescriptorSets");

  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = kPathtraceSpvWords * sizeof(uint32_t);
  smci.pCode = kPathtraceSpv;
  VkShaderModule module = VK_NULL_HANDLE;
  check(vk_.vkCreateShaderModule(device_, &smci, nullptr, &module), "vkCreateShaderModule");

  const auto stage = [&](VkShaderStageFlagBits s, const char* name) {
    VkPipelineShaderStageCreateInfo x{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    x.stage = s;
    x.module = module;
    x.pName = name;
    return x;
  };
  const VkPipelineShaderStageCreateInfo stages[] = {
      stage(VK_SHADER_STAGE_RAYGEN_BIT_KHR, "raygen"), stage(VK_SHADER_STAGE_MISS_BIT_KHR, "miss"),
      stage(VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, "closestHit"), stage(VK_SHADER_STAGE_ANY_HIT_BIT_KHR, "anyHit")};
  VkRayTracingShaderGroupCreateInfoKHR groups[3]{};
  for (auto& g : groups) {
    g.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    g.generalShader = g.closestHitShader = g.anyHitShader = g.intersectionShader = VK_SHADER_UNUSED_KHR;
  }
  groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  groups[0].generalShader = 0;
  groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
  groups[1].generalShader = 1;
  groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  groups[2].closestHitShader = 2;
  groups[2].anyHitShader = 3;

  VkRayTracingPipelineCreateInfoKHR rpci{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
  rpci.stageCount = 4;
  rpci.pStages = stages;
  rpci.groupCount = 3;
  rpci.pGroups = groups;
  rpci.maxPipelineRayRecursionDepth = 1;
  rpci.layout = pipeline_layout_;
  const VkResult r = vk_.vkCreateRayTracingPipelinesKHR(device_, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rpci, nullptr,
                                                         &pipeline_);
  vk_.vkDestroyShaderModule(device_, module, nullptr);
  check(r, "vkCreateRayTracingPipelinesKHR");

  // Shader binding table: raygen | miss | hit group, each region aligned to the base alignment.
  const uint32_t handle = rt_props_.shaderGroupHandleSize;
  const VkDeviceSize stride = alignUp(handle, rt_props_.shaderGroupHandleAlignment);
  const VkDeviceSize base = rt_props_.shaderGroupBaseAlignment;
  std::vector<uint8_t> handles(size_t(handle) * 3);
  check(vk_.vkGetRayTracingShaderGroupHandlesKHR(device_, pipeline_, 0, 3, handles.size(), handles.data()),
        "vkGetRayTracingShaderGroupHandlesKHR");
  const VkDeviceSize region = alignUp(stride, base);
  sbt_ = createBuffer(region * 3 + base, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR, Memory::Upload);
  const VkDeviceAddress start = alignUp(sbt_.address, base);
  auto* bytes = static_cast<uint8_t*>(sbt_.mapped) + (start - sbt_.address);
  for (int i = 0; i < 3; ++i) std::memcpy(bytes + region * i, handles.data() + size_t(handle) * i, handle);
  vmaFlushAllocation(vma_, sbt_.alloc, 0, VK_WHOLE_SIZE);
  rgen_ = {start, stride, stride};
  miss_ = {start + region, stride, stride};
  hit_ = {start + region * 2, stride, stride};
}

void VulkanBackend::writeDescriptors() {
  std::vector<VkWriteDescriptorSet> writes;
  std::vector<VkDescriptorBufferInfo> infos(kBindCount);
  VkWriteDescriptorSetAccelerationStructureKHR asw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  asw.accelerationStructureCount = 1;
  asw.pAccelerationStructures = &tlas_;
  for (uint32_t i = 0; i < kBindCount; ++i) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = i;
    w.descriptorCount = 1;
    if (i == kBindTlas) {
      w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
      w.pNext = &asw;
    } else {
      const Buffer& buf = i == kBindFrame ? frame_buffer_
                          : i == kBindOut ? out_buffer_
                          : i == kBindCounters ? counter_buffer_
                                               : scene_buffers_[i];
      infos[i] = {buf.buffer, 0, VK_WHOLE_SIZE};
      w.descriptorType = i == kBindFrame ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w.pBufferInfo = &infos[i];
    }
    writes.push_back(w);
  }
  vk_.vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

// ---------------------------------------------------------------- frames and items

void VulkanBackend::prepare(const FrameSetup& frame) {
  cancel_ = false;
  if (!scene_ready_) throw BackendError("Vulkan: prepare() before upload()");
  if (frame.params.mode == Integrator::DebugBvh)
    throw BackendError("debug:bvh needs traversal step counts, which Vulkan ray tracing does not report; use "
                       "--backend cuda");
  const gpu::Frame f = makeGpuFrame(frame, *scene_);
  std::memcpy(frame_buffer_.mapped, &f, sizeof(f));
  vmaFlushAllocation(vma_, frame_buffer_.alloc, 0, VK_WHOLE_SIZE);
  ensureOutput(256 * 256);
  writeDescriptors();
}

void VulkanBackend::ensureOutput(uint64_t pixels) {
  if (pixels <= out_capacity_ && out_buffer_.buffer) return;
  vk_.vkDeviceWaitIdle(device_);
  destroy(out_buffer_);
  out_buffer_ = createBuffer(pixels * sizeof(Vec3f), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Memory::Readback);
  out_capacity_ = pixels;
  if (scene_ready_) writeDescriptors();
}

void VulkanBackend::render(const WorkItem& item, TileResult& out) {
  const auto start = std::chrono::steady_clock::now();
  const TileRect r = item.rect;
  out.item_id = item.id;
  out.rect = r;
  out.sample_begin = item.sample_begin;
  out.sample_end = item.sample_end;
  out.mean.assign(r.pixels(), Vec3f(0));
  if (cancel_) {
    out.cancelled = true;
    return;
  }
  ensureOutput(r.pixels());
  std::memset(counter_buffer_.mapped, 0, 16);
  vmaFlushAllocation(vma_, counter_buffer_.alloc, 0, VK_WHOLE_SIZE);
  const gpu::PushConstants pc{r.x0, r.y0, r.width(), r.height(), item.sample_begin, item.sample_end,
                              item.sample_end - item.sample_begin, gpu::kItemFinish};
  submitAndWait([&](VkCommandBuffer c) {
    vk_.vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline_);
    vk_.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline_layout_, 0, 1, &set_, 0, nullptr);
    vk_.vkCmdPushConstants(c, pipeline_layout_, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(pc), &pc);
    vk_.vkCmdTraceRaysKHR(c, &rgen_, &miss_, &hit_, &call_, r.width(), r.height(), 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vk_.vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
  });
  vmaInvalidateAllocation(vma_, out_buffer_.alloc, 0, VK_WHOLE_SIZE);
  vmaInvalidateAllocation(vma_, counter_buffer_.alloc, 0, VK_WHOLE_SIZE);
  std::memcpy(out.mean.data(), out_buffer_.mapped, r.pixels() * sizeof(Vec3f));
  out.nan_count = static_cast<const uint32_t*>(counter_buffer_.mapped)[0];
  out.cancelled = cancel_.load();
  out.render_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// ---------------------------------------------------------------- parity test

std::vector<float> VulkanBackend::runParity(uint32_t count) {
  constexpr uint32_t kValues = 32;
  const std::vector<float> sheen(sheenAlbedoTable(), sheenAlbedoTable() + kSheenTableSize * kSheenTableSize);
  Buffer sheenBuf = deviceBuffer(sheen, 0);
  Buffer out = createBuffer(VkDeviceSize(count) * kValues * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            Memory::Readback);
  VkDescriptorSetLayoutBinding b[2]{};
  b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  b[1] = {20, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.bindingCount = 2;
  lci.pBindings = b;
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  check(vk_.vkCreateDescriptorSetLayout(device_, &lci, nullptr, &layout), "vkCreateDescriptorSetLayout");
  VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &layout;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pcr;
  VkPipelineLayout pl = VK_NULL_HANDLE;
  check(vk_.vkCreatePipelineLayout(device_, &plci, nullptr, &pl), "vkCreatePipelineLayout");
  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = kParitySpvWords * sizeof(uint32_t);
  smci.pCode = kParitySpv;
  VkShaderModule module = VK_NULL_HANDLE;
  check(vk_.vkCreateShaderModule(device_, &smci, nullptr, &module), "vkCreateShaderModule");
  VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module,
                "parity", nullptr};
  cpci.layout = pl;
  VkPipeline pipe = VK_NULL_HANDLE;
  const VkResult r = vk_.vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe);
  vk_.vkDestroyShaderModule(device_, module, nullptr);
  check(r, "vkCreateComputePipelines");
  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
  VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &ps;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  check(vk_.vkCreateDescriptorPool(device_, &dpci, nullptr, &pool), "vkCreateDescriptorPool");
  VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = pool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  check(vk_.vkAllocateDescriptorSets(device_, &dsai, &set), "vkAllocateDescriptorSets");
  VkDescriptorBufferInfo infos[2] = {{out.buffer, 0, VK_WHOLE_SIZE}, {sheenBuf.buffer, 0, VK_WHOLE_SIZE}};
  VkWriteDescriptorSet w[2]{};
  for (int i = 0; i < 2; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = set;
    w[i].dstBinding = b[i].binding;
    w[i].descriptorCount = 1;
    w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[i].pBufferInfo = &infos[i];
  }
  vk_.vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
  submitAndWait([&](VkCommandBuffer c) {
    vk_.vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vk_.vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, nullptr);
    vk_.vkCmdPushConstants(c, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &count);
    vk_.vkCmdDispatch(c, (count + 63) / 64, 1, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vk_.vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr,
                             0, nullptr);
  });
  vmaInvalidateAllocation(vma_, out.alloc, 0, VK_WHOLE_SIZE);
  std::vector<float> result(size_t(count) * kValues);
  std::memcpy(result.data(), out.mapped, result.size() * sizeof(float));
  vk_.vkDestroyPipeline(device_, pipe, nullptr);
  vk_.vkDestroyDescriptorPool(device_, pool, nullptr);
  vk_.vkDestroyPipelineLayout(device_, pl, nullptr);
  vk_.vkDestroyDescriptorSetLayout(device_, layout, nullptr);
  destroy(out);
  destroy(sheenBuf);
  return result;
}

}  // namespace

std::vector<float> runVulkanParity(uint32_t physicalIndex, uint32_t count) {
  VulkanBackend b(physicalIndex);
  return b.runParity(count);
}

const VulkanProbe& probeVulkan() { return instanceState().probe; }

std::unique_ptr<IBackend> createVulkanBackend(uint32_t physicalIndex) {
  return std::make_unique<VulkanBackend>(physicalIndex);
}

}  // namespace pen
