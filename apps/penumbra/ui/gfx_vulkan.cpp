// The UI's Vulkan renderer: its own instance and device with a swapchain, independent of the render backends'
// (which have no surface). Adapted from Dear ImGui's example_glfw_vulkan; errors throw so the app can fall back to
// OpenGL instead of aborting.

#include "apps/penumbra/ui/gfx.h"

#include <volk.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <GLFW/glfw3.h>

#include <cstring>
#include <fmt/format.h>
#include <stdexcept>
#include <vector>

namespace pen::ui {

namespace {

void check(VkResult r, const char* what) {
  if (r != VK_SUCCESS) throw std::runtime_error(fmt::format("Vulkan: {} failed ({})", what, static_cast<int>(r)));
}

// ImGui's backend reports errors through a plain function: frame errors after setup are logged by the caller's
// next check, so this only needs to not abort.
void ignoreVkResult(VkResult) {}

bool hasExtension(const std::vector<VkExtensionProperties>& props, const char* name) {
  for (const auto& p : props)
    if (std::strcmp(p.extensionName, name) == 0) return true;
  return false;
}

class VulkanGfx final : public Gfx {
 public:
  explicit VulkanGfx(GLFWwindow* window) : window_(window) {
    try {
      setup();
    } catch (...) {
      release();
      throw;
    }
  }
  ~VulkanGfx() override {
    if (device_) vkDeviceWaitIdle(device_);
    release();
  }
  const char* name() const override { return "Vulkan"; }

  void newFrame() override {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    if (w > 0 && h > 0 && (rebuild_ || wd_.Width != w || wd_.Height != h)) {
      ImGui_ImplVulkan_SetMinImageCount(kMinImageCount);
      ImGui_ImplVulkanH_CreateOrResizeWindow(instance_, physical_, device_, &wd_, queue_family_, nullptr, w, h,
                                             kMinImageCount, 0);
      wd_.FrameIndex = 0;
      rebuild_ = false;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
  }

  void render(const float clear[4]) override {
    ImDrawData* draw = ImGui::GetDrawData();
    if (draw->DisplaySize.x <= 0 || draw->DisplaySize.y <= 0) return;  // minimized
    for (int i = 0; i < 4; ++i) wd_.ClearValue.color.float32[i] = clear[i];

    VkSemaphore acquired = wd_.FrameSemaphores[wd_.SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore done = wd_.FrameSemaphores[wd_.SemaphoreIndex].RenderCompleteSemaphore;
    VkResult r = vkAcquireNextImageKHR(device_, wd_.Swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &wd_.FrameIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) rebuild_ = true;
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return;
    if (r != VK_SUBOPTIMAL_KHR) check(r, "vkAcquireNextImageKHR");

    ImGui_ImplVulkanH_Frame* fd = &wd_.Frames[wd_.FrameIndex];
    check(vkWaitForFences(device_, 1, &fd->Fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    check(vkResetFences(device_, 1, &fd->Fence), "vkResetFences");
    check(vkResetCommandPool(device_, fd->CommandPool, 0), "vkResetCommandPool");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(fd->CommandBuffer, &begin), "vkBeginCommandBuffer");
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = wd_.RenderPass;
    rp.framebuffer = fd->Framebuffer;
    rp.renderArea.extent.width = static_cast<uint32_t>(wd_.Width);
    rp.renderArea.extent.height = static_cast<uint32_t>(wd_.Height);
    rp.clearValueCount = 1;
    rp.pClearValues = &wd_.ClearValue;
    vkCmdBeginRenderPass(fd->CommandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(draw, fd->CommandBuffer);
    vkCmdEndRenderPass(fd->CommandBuffer);
    check(vkEndCommandBuffer(fd->CommandBuffer), "vkEndCommandBuffer");

    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired;
    submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &fd->CommandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &done;
    check(vkQueueSubmit(queue_, 1, &submit, fd->Fence), "vkQueueSubmit");

    if (rebuild_) return;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &done;
    present.swapchainCount = 1;
    present.pSwapchains = &wd_.Swapchain;
    present.pImageIndices = &wd_.FrameIndex;
    r = vkQueuePresentKHR(queue_, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) rebuild_ = true;
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return;
    if (r != VK_SUBOPTIMAL_KHR) check(r, "vkQueuePresentKHR");
    wd_.SemaphoreIndex = (wd_.SemaphoreIndex + 1) % wd_.SemaphoreCount;
  }

 private:
  static constexpr uint32_t kMinImageCount = 2;

  void setup() {
    check(volkInitialize(), "loading the Vulkan loader");
    glfwInitVulkanLoader(vkGetInstanceProcAddr);
    if (!glfwVulkanSupported()) throw std::runtime_error("GLFW: no Vulkan support for windows");

    uint32_t count = 0;
    const char** glfwExt = glfwGetRequiredInstanceExtensions(&count);
    if (!glfwExt) throw std::runtime_error("GLFW: no Vulkan surface extensions");
    std::vector<const char*> extensions(glfwExt, glfwExt + count);
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, props.data());
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (hasExtension(props, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
      extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    if (hasExtension(props, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
      extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
      ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Penumbra UI";
    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();
    check(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance");
    // Instance-level and device-level entry points through the loader's trampolines: they serve every instance and
    // device, so the render backends' own Vulkan objects are unaffected.
    volkLoadInstance(instance_);

    check(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_), "glfwCreateWindowSurface");

    // The first GPU that can present to this window (discrete preferred by ImGui's selection).
    physical_ = ImGui_ImplVulkanH_SelectPhysicalDevice(instance_);
    if (physical_ == VK_NULL_HANDLE) throw std::runtime_error("Vulkan: no GPU");
    queue_family_ = ImGui_ImplVulkanH_SelectQueueFamilyIndex(physical_);
    if (queue_family_ == static_cast<uint32_t>(-1)) throw std::runtime_error("Vulkan: no graphics queue");
    VkBool32 canPresent = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physical_, queue_family_, surface_, &canPresent);
    if (!canPresent) throw std::runtime_error("Vulkan: the GPU cannot present to this window");

    const char* deviceExt[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = deviceExt;
    check(vkCreateDevice(physical_, &dci, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE},
        {VK_DESCRIPTOR_TYPE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},  // the viewport's images
    };
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    for (const auto& s : sizes) pci.maxSets += s.descriptorCount;
    pci.poolSizeCount = static_cast<uint32_t>(IM_COUNTOF(sizes));
    pci.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(device_, &pci, nullptr, &pool_), "vkCreateDescriptorPool");

    const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM,
                                VK_FORMAT_R8G8B8_UNORM};
    wd_.Surface = surface_;
    wd_.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(physical_, surface_, formats, IM_COUNTOF(formats),
                                                              VK_COLORSPACE_SRGB_NONLINEAR_KHR);
    VkPresentModeKHR modes[] = {VK_PRESENT_MODE_FIFO_KHR};
    wd_.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(physical_, surface_, modes, IM_COUNTOF(modes));
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    ImGui_ImplVulkanH_CreateOrResizeWindow(instance_, physical_, device_, &wd_, queue_family_, nullptr, w, h,
                                           kMinImageCount, 0);
    window_created_ = true;

    if (!ImGui_ImplGlfw_InitForVulkan(window_, true)) throw std::runtime_error("ImGui GLFW backend failed");
    imgui_glfw_ = true;
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_1;
    init.Instance = instance_;
    init.PhysicalDevice = physical_;
    init.Device = device_;
    init.QueueFamily = queue_family_;
    init.Queue = queue_;
    init.DescriptorPool = pool_;
    init.MinImageCount = kMinImageCount;
    init.ImageCount = wd_.ImageCount;
    init.PipelineInfoMain.RenderPass = wd_.RenderPass;
    init.PipelineInfoMain.Subpass = 0;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.CheckVkResultFn = ignoreVkResult;
    if (!ImGui_ImplVulkan_Init(&init)) throw std::runtime_error("ImGui Vulkan backend failed");
    imgui_vulkan_ = true;
  }

  // Undoes whatever setup() got to, in reverse order (also after a failed setup).
  void release() {
    if (imgui_vulkan_) ImGui_ImplVulkan_Shutdown();
    if (imgui_glfw_) ImGui_ImplGlfw_Shutdown();
    imgui_vulkan_ = imgui_glfw_ = false;
    if (window_created_) ImGui_ImplVulkanH_DestroyWindow(instance_, device_, &wd_, nullptr);
    window_created_ = false;
    if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
    if (device_) vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (instance_) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE;
  }

  GLFWwindow* window_;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  ImGui_ImplVulkanH_Window wd_;
  bool window_created_ = false;
  bool imgui_glfw_ = false;
  bool imgui_vulkan_ = false;
  bool rebuild_ = false;
};

}  // namespace

std::unique_ptr<Gfx> createVulkanGfx(GLFWwindow* window) { return std::make_unique<VulkanGfx>(window); }

}  // namespace pen::ui
