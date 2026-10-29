#include "backends/cuda/cuda_backend.h"

#include <cuda.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fmt/format.h>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "backends/gpu/gpu_bvh.h"
#include "backends/gpu/gpu_scene.h"
#include "core/settings/parse.h"
#include "core/shading/bsdf.h"
#include "parity_cuda_layout.h"
#include "pathtrace_cuda_layout.h"

namespace pen {

// Kernels of shaders/pathtrace_cuda.slang and shaders/parity.slang, compiled at build time (DECISIONS D-087).
extern const unsigned char* const kPathtraceFatbin;
extern const size_t kPathtraceFatbinSize;
extern const unsigned char* const kParityFatbin;
extern const size_t kParityFatbinSize;

namespace {

// ---------------------------------------------------------------- layout checks (DECISIONS D-086)

namespace L = cuda_layout::pathtrace;

constexpr uint32_t kMissing = 0xffffffffu;

constexpr uint32_t fieldOffset(std::string_view type, std::string_view name) {
  for (const L::Field& f : L::kFields)
    if (f.type == type && f.name == name) return f.offset;
  return kMissing;
}

constexpr uint32_t structSize(std::string_view type) {
  for (const L::Struct& s : L::kStructs)
    if (s.name == type) return s.size;
  return kMissing;
}

#define PEN_CUDA_FIELD(T, S, f)                                                                                 \
  static_assert(fieldOffset(S, #f) == offsetof(gpu::T, f), "gpu::" #T "::" #f " is not where the CUDA kernel " \
                                                           "reads it (backends/gpu/gpu_types.h vs shaders/types.slang)")
#define PEN_CUDA_SIZE(T, S) \
  static_assert(structSize(S) == sizeof(gpu::T), "gpu::" #T " and the CUDA kernel's " S " differ in size")

PEN_CUDA_SIZE(TextureRef, "TextureRef");
PEN_CUDA_FIELD(TextureRef, "TextureRef", index);
PEN_CUDA_FIELD(TextureRef, "TextureRef", tex_coord);
PEN_CUDA_FIELD(TextureRef, "TextureRef", offset);
PEN_CUDA_FIELD(TextureRef, "TextureRef", has_transform);
PEN_CUDA_FIELD(TextureRef, "TextureRef", rotation);
PEN_CUDA_FIELD(TextureRef, "TextureRef", scale);
PEN_CUDA_SIZE(Material, "Material");
PEN_CUDA_FIELD(Material, "Material", base_color);
PEN_CUDA_FIELD(Material, "Material", base_color_tex);
PEN_CUDA_FIELD(Material, "Material", metallic_roughness_tex);
PEN_CUDA_FIELD(Material, "Material", normal_tex);
PEN_CUDA_FIELD(Material, "Material", emissive_tex);
PEN_CUDA_FIELD(Material, "Material", transmission_tex);
PEN_CUDA_FIELD(Material, "Material", specular_tex);
PEN_CUDA_FIELD(Material, "Material", specular_color_tex);
PEN_CUDA_FIELD(Material, "Material", clearcoat_tex);
PEN_CUDA_FIELD(Material, "Material", clearcoat_roughness_tex);
PEN_CUDA_FIELD(Material, "Material", sheen_color_tex);
PEN_CUDA_FIELD(Material, "Material", sheen_roughness_tex);
PEN_CUDA_FIELD(Material, "Material", anisotropy_tex);
PEN_CUDA_FIELD(Material, "Material", metallic);
PEN_CUDA_FIELD(Material, "Material", roughness);
PEN_CUDA_FIELD(Material, "Material", normal_scale);
PEN_CUDA_FIELD(Material, "Material", emissive);
PEN_CUDA_FIELD(Material, "Material", emissive_strength);
PEN_CUDA_FIELD(Material, "Material", transmission);
PEN_CUDA_FIELD(Material, "Material", ior);
PEN_CUDA_FIELD(Material, "Material", specular);
PEN_CUDA_FIELD(Material, "Material", specular_color);
PEN_CUDA_FIELD(Material, "Material", clearcoat);
PEN_CUDA_FIELD(Material, "Material", clearcoat_roughness);
PEN_CUDA_FIELD(Material, "Material", sheen_color);
PEN_CUDA_FIELD(Material, "Material", sheen_roughness);
PEN_CUDA_FIELD(Material, "Material", anisotropy);
PEN_CUDA_FIELD(Material, "Material", anisotropy_rotation);
PEN_CUDA_FIELD(Material, "Material", alpha_mode);
PEN_CUDA_FIELD(Material, "Material", alpha_cutoff);
PEN_CUDA_FIELD(Material, "Material", double_sided);
PEN_CUDA_FIELD(Material, "Material", emits);
PEN_CUDA_SIZE(Texture, "Texture");
PEN_CUDA_FIELD(Texture, "Texture", width);
PEN_CUDA_FIELD(Texture, "Texture", height);
PEN_CUDA_FIELD(Texture, "Texture", is_float);
PEN_CUDA_FIELD(Texture, "Texture", wrap_s);
PEN_CUDA_FIELD(Texture, "Texture", wrap_t);
PEN_CUDA_FIELD(Texture, "Texture", nearest);
PEN_CUDA_FIELD(Texture, "Texture", offset);
PEN_CUDA_SIZE(Primitive, "Primitive");
PEN_CUDA_FIELD(Primitive, "Primitive", index_offset);
PEN_CUDA_FIELD(Primitive, "Primitive", position_offset);
PEN_CUDA_FIELD(Primitive, "Primitive", normal_offset);
PEN_CUDA_FIELD(Primitive, "Primitive", tangent_offset);
PEN_CUDA_FIELD(Primitive, "Primitive", uv0_offset);
PEN_CUDA_FIELD(Primitive, "Primitive", uv1_offset);
PEN_CUDA_FIELD(Primitive, "Primitive", color_offset);
PEN_CUDA_FIELD(Primitive, "Primitive", material);
PEN_CUDA_SIZE(Instance, "Instance");
PEN_CUDA_FIELD(Instance, "Instance", to_world);
PEN_CUDA_FIELD(Instance, "Instance", inverse);
PEN_CUDA_FIELD(Instance, "Instance", mesh);
PEN_CUDA_FIELD(Instance, "Instance", primitive_base);
PEN_CUDA_FIELD(Instance, "Instance", slot_base);
PEN_CUDA_SIZE(EmissiveTriangle, "EmissiveTriangle");
PEN_CUDA_FIELD(EmissiveTriangle, "EmissiveTriangle", p0);
PEN_CUDA_FIELD(EmissiveTriangle, "EmissiveTriangle", e1);
PEN_CUDA_FIELD(EmissiveTriangle, "EmissiveTriangle", e2);
PEN_CUDA_FIELD(EmissiveTriangle, "EmissiveTriangle", normal);
PEN_CUDA_FIELD(EmissiveTriangle, "EmissiveTriangle", area);
PEN_CUDA_FIELD(EmissiveTriangle, "EmissiveTriangle", instance);
PEN_CUDA_FIELD(EmissiveTriangle, "EmissiveTriangle", primitive);
PEN_CUDA_FIELD(EmissiveTriangle, "EmissiveTriangle", triangle);
PEN_CUDA_FIELD(EmissiveTriangle, "EmissiveTriangle", double_sided);
PEN_CUDA_SIZE(AliasEntry, "AliasEntry");
PEN_CUDA_FIELD(AliasEntry, "AliasEntry", prob);
PEN_CUDA_FIELD(AliasEntry, "AliasEntry", alias);
PEN_CUDA_FIELD(AliasEntry, "AliasEntry", pmf);
PEN_CUDA_SIZE(PunctualLight, "PunctualLight");
PEN_CUDA_FIELD(PunctualLight, "PunctualLight", type);
PEN_CUDA_FIELD(PunctualLight, "PunctualLight", position);
PEN_CUDA_FIELD(PunctualLight, "PunctualLight", direction);
PEN_CUDA_FIELD(PunctualLight, "PunctualLight", intensity);
PEN_CUDA_FIELD(PunctualLight, "PunctualLight", cos_inner);
PEN_CUDA_FIELD(PunctualLight, "PunctualLight", cos_outer);
PEN_CUDA_SIZE(Frame, "FrameData");
PEN_CUDA_FIELD(Frame, "FrameData", projection);
PEN_CUDA_FIELD(Frame, "FrameData", cam_to_world);
PEN_CUDA_FIELD(Frame, "FrameData", ymag);
PEN_CUDA_FIELD(Frame, "FrameData", aperture_radius);
PEN_CUDA_FIELD(Frame, "FrameData", focus_distance);
PEN_CUDA_FIELD(Frame, "FrameData", width);
PEN_CUDA_FIELD(Frame, "FrameData", height);
PEN_CUDA_FIELD(Frame, "FrameData", tan_x);
PEN_CUDA_FIELD(Frame, "FrameData", tan_y);
PEN_CUDA_FIELD(Frame, "FrameData", mode);
PEN_CUDA_FIELD(Frame, "FrameData", max_bounces);
PEN_CUDA_FIELD(Frame, "FrameData", max_diffuse);
PEN_CUDA_FIELD(Frame, "FrameData", max_glossy);
PEN_CUDA_FIELD(Frame, "FrameData", max_transmission);
PEN_CUDA_FIELD(Frame, "FrameData", russian_roulette);
PEN_CUDA_FIELD(Frame, "FrameData", ao_distance);
PEN_CUDA_FIELD(Frame, "FrameData", seed);
PEN_CUDA_FIELD(Frame, "FrameData", num_emissive);
PEN_CUDA_FIELD(Frame, "FrameData", num_punctual);
PEN_CUDA_FIELD(Frame, "FrameData", light_count);
PEN_CUDA_FIELD(Frame, "FrameData", light_sum);
PEN_CUDA_FIELD(Frame, "FrameData", env_has_map);
PEN_CUDA_FIELD(Frame, "FrameData", env_width);
PEN_CUDA_FIELD(Frame, "FrameData", env_height);
PEN_CUDA_FIELD(Frame, "FrameData", env_strength);
PEN_CUDA_FIELD(Frame, "FrameData", env_rotation);
PEN_CUDA_FIELD(Frame, "FrameData", env_color);
PEN_CUDA_FIELD(Frame, "FrameData", env_is_black);
PEN_CUDA_FIELD(Frame, "FrameData", env_dist_sum);
PEN_CUDA_FIELD(Frame, "FrameData", medium_count);
PEN_CUDA_FIELD(Frame, "FrameData", medium);
// MediumLayer is not reported on its own (only nested in FrameData's array): its 40 bytes of plain 4-byte fields
// are checked by FrameData's size (296) and the offset of the array.
PEN_CUDA_FIELD(Frame, "FrameData", env_light_index);
PEN_CUDA_SIZE(PushConstants, "PushConstants");
PEN_CUDA_FIELD(PushConstants, "PushConstants", x0);
PEN_CUDA_FIELD(PushConstants, "PushConstants", y0);
PEN_CUDA_FIELD(PushConstants, "PushConstants", width);
PEN_CUDA_FIELD(PushConstants, "PushConstants", height);
PEN_CUDA_FIELD(PushConstants, "PushConstants", sample_begin);
PEN_CUDA_FIELD(PushConstants, "PushConstants", sample_end);
PEN_CUDA_FIELD(PushConstants, "PushConstants", sample_count);
PEN_CUDA_FIELD(PushConstants, "PushConstants", flags);
PEN_CUDA_SIZE(Blas, "Blas");
PEN_CUDA_FIELD(Blas, "Blas", kind);
PEN_CUDA_FIELD(Blas, "Blas", node_base);
PEN_CUDA_FIELD(Blas, "Blas", tri_base);
PEN_CUDA_FIELD(Blas, "Blas", tri_count);
PEN_CUDA_SIZE(TlasNode, "TlasNode");
PEN_CUDA_FIELD(TlasNode, "TlasNode", lo);
PEN_CUDA_FIELD(TlasNode, "TlasNode", left_first);
PEN_CUDA_FIELD(TlasNode, "TlasNode", hi);
PEN_CUDA_FIELD(TlasNode, "TlasNode", count);

#undef PEN_CUDA_FIELD
#undef PEN_CUDA_SIZE

// ---------------------------------------------------------------- driver (loaded at run time)

#define PEN_CU_STR2(x) #x
#define PEN_CU_STR(x) PEN_CU_STR2(x)  // expands cuda.h's versioned names first: cuMemAlloc -> "cuMemAlloc_v2"

// The driver API entry points Penumbra uses; names are cuda.h's, so the macros pick the versioned symbols.
#define PEN_CU_FUNCTIONS(X)                                                                                  \
  X(cuInit) X(cuDriverGetVersion) X(cuDeviceGetCount) X(cuDeviceGet) X(cuDeviceGetName) X(cuDeviceGetAttribute) \
  X(cuDeviceTotalMem) X(cuDeviceGetUuid) X(cuDevicePrimaryCtxRetain) X(cuDevicePrimaryCtxRelease)             \
  X(cuCtxPushCurrent) X(cuCtxPopCurrent) X(cuCtxSynchronize) X(cuModuleLoadDataEx) X(cuModuleUnload)          \
  X(cuModuleGetFunction) X(cuModuleGetGlobal) X(cuMemAlloc) X(cuMemFree) X(cuMemcpyHtoD) X(cuMemcpyDtoH)      \
  X(cuMemsetD8) X(cuLaunchKernel) X(cuGetErrorName) X(cuGetErrorString) X(cuMemGetInfo) X(cuStreamQuery)       \
  X(cuMemHostAlloc) X(cuMemFreeHost) X(cuMemHostGetDevicePointer)

struct Driver {
#define PEN_CU_MEMBER(name) decltype(&::name) name = nullptr;
  PEN_CU_FUNCTIONS(PEN_CU_MEMBER)
#undef PEN_CU_MEMBER
};

struct DriverState {
  Driver cu;
  CudaProbe probe;
};

#if defined(_WIN32)
constexpr const char* kDriverLibrary = "nvcuda.dll";
void* openLibrary() { return reinterpret_cast<void*>(LoadLibraryA(kDriverLibrary)); }
void* librarySymbol(void* lib, const char* name) {
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
}
#else
constexpr const char* kDriverLibrary = "libcuda.so.1";
void* openLibrary() { return dlopen(kDriverLibrary, RTLD_NOW | RTLD_LOCAL); }
void* librarySymbol(void* lib, const char* name) { return dlsym(lib, name); }
#endif

std::string errorText(const Driver& cu, CUresult r) {
  const char* name = nullptr;
  const char* text = nullptr;
  if (cu.cuGetErrorName) cu.cuGetErrorName(r, &name);
  if (cu.cuGetErrorString) cu.cuGetErrorString(r, &text);
  return fmt::format("{} ({})", text ? text : "unknown error", name ? name : std::to_string(static_cast<int>(r)));
}

// The architectures this build has machine code for (PEN_CUDA_ARCHS minus what the toolkit lacks) and the
// virtual architecture of the embedded PTX; set by backends/CMakeLists.txt.
constexpr int kSass[] = {PEN_CUDA_SASS};
constexpr int kPtxArch = PEN_CUDA_PTX;
constexpr int kToolkitVersion = CUDA_VERSION;  // e.g. 13000

// Why this build cannot run on a GPU of compute capability major.minor with this driver, or "" if it can.
std::string whyUnusable(int major, int minor, int driverVersion) {
  if (driverVersion / 1000 < kToolkitVersion / 1000)
    return fmt::format("the driver supports CUDA {}.{}, this build needs CUDA {}.x (update the NVIDIA driver, or "
                       "build Penumbra with a CUDA {}.x toolkit)",
                       driverVersion / 1000, driverVersion % 1000 / 10, kToolkitVersion / 1000, driverVersion / 1000);
  const int cc = major * 10 + minor;
  for (int a : kSass)
    if (a / 10 == major && a % 10 <= minor) return "";  // machine code runs on later minor versions
  if (cc >= kPtxArch) return "";                        // the driver compiles the PTX
  return fmt::format("compute capability {}.{} is older than this build's oldest target ({}); add {} to "
                     "PEN_CUDA_ARCHS and rebuild with a CUDA toolkit that still supports it",
                     major, minor, kPtxArch / 10.0, cc);
}

DriverState& driverState() {
  static DriverState state = [] {
    DriverState s;
    void* lib = openLibrary();
    if (!lib) {
      s.probe.error = fmt::format("no CUDA driver ({} not found)", kDriverLibrary);
      return s;
    }
    std::string missing;
#define PEN_CU_LOAD(name)                                                                       \
  s.cu.name = reinterpret_cast<decltype(s.cu.name)>(librarySymbol(lib, PEN_CU_STR(name)));      \
  if (!s.cu.name) missing += std::string(missing.empty() ? "" : ", ") + PEN_CU_STR(name);
    PEN_CU_FUNCTIONS(PEN_CU_LOAD)
#undef PEN_CU_LOAD
    if (!missing.empty()) {
      s.probe.error = fmt::format("the CUDA driver is too old (missing {})", missing);
      s.cu = Driver{};
      return s;
    }
    const CUresult init = s.cu.cuInit(0);
    if (init != CUDA_SUCCESS) {
      s.probe.error = fmt::format("cuInit failed: {}", errorText(s.cu, init));
      return s;
    }
    s.probe.driver_found = true;
    s.cu.cuDriverGetVersion(&s.probe.driver_version);
    int count = 0;
    s.cu.cuDeviceGetCount(&count);
    for (int i = 0; i < count; ++i) {
      CUdevice dev;
      if (s.cu.cuDeviceGet(&dev, i) != CUDA_SUCCESS) continue;
      CudaGpu g;
      g.ordinal = static_cast<uint32_t>(i);
      char name[256] = {};
      s.cu.cuDeviceGetName(name, sizeof(name), dev);
      g.name = name;
      s.cu.cuDeviceGetAttribute(&g.cc_major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev);
      s.cu.cuDeviceGetAttribute(&g.cc_minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);
      size_t mem = 0;
      s.cu.cuDeviceTotalMem(&mem, dev);
      g.vram_bytes = mem;
      CUuuid uuid{};
      if (s.cu.cuDeviceGetUuid(&uuid, dev) == CUDA_SUCCESS) std::memcpy(g.uuid.data(), uuid.bytes, 16);
      g.why = whyUnusable(g.cc_major, g.cc_minor, s.probe.driver_version);
      g.usable = g.why.empty();
      s.probe.gpus.push_back(g);
    }
    return s;
  }();
  return state;
}

// Makes the device's context current on this thread for the scope.
class CtxScope {
 public:
  CtxScope(const Driver& cu, CUcontext ctx) : cu_(cu) { cu_.cuCtxPushCurrent(ctx); }
  ~CtxScope() {
    CUcontext popped;
    cu_.cuCtxPopCurrent(&popped);
  }
  CtxScope(const CtxScope&) = delete;
  CtxScope& operator=(const CtxScope&) = delete;

 private:
  const Driver& cu_;
};

// A kernel module with its global parameter block (SLANG_globalParams), filled by parameter name.
struct Module {
  CUmodule module = nullptr;
  CUfunction function = nullptr;
  CUdeviceptr globals = 0;
  size_t globals_size = 0;
  std::vector<uint8_t> host;  // host copy of the parameter block
};

struct DeviceBuffer {
  CUdeviceptr ptr = 0;
  size_t bytes = 0;
  void* host = nullptr;  // pinned RAM mapped into the GPU's address space (memory tiers 1 and 2), else null
};

// ---------------------------------------------------------------- backend

class CudaBackend final : public IBackend {
 public:
  explicit CudaBackend(uint32_t ordinal);
  ~CudaBackend() override;

  DeviceInfo info() const override { return info_; }
  uint64_t vramBudget() override;
  SceneFootprint measure(const Scene& scene) override;
  void upload(const Scene& scene, const Placement& placement) override;
  void prepare(const FrameSetup& frame) override;
  void render(const WorkItem& item, TileResult& out) override;
  void cancel() override { cancel_ = true; }
  std::vector<float> runParity(uint32_t count);

 private:
  void check(CUresult r, const char* what) const {
    if (r != CUDA_SUCCESS) throw BackendError(fmt::format("CUDA on {}: {} failed: {}", info_.name, what, errorText(cu_, r)));
  }
  void synchronize(const char* what);
  Module loadModule(const unsigned char* image, const char* kernel, uint32_t globalsSize);
  void unloadModule(Module& m);
  DeviceBuffer allocate(size_t bytes);
  // Pinned RAM the GPU reads over PCIe (cuMemHostAlloc, mapped): explicit on both OSes, never managed memory,
  // which Windows cannot oversubscribe (docs/GUIDE.md "Platform gotchas").
  DeviceBuffer allocateHost(size_t bytes);
  void release(DeviceBuffer& b);
  // Uploads an array and points the named StructuredBuffer at it ({T* data; size_t count} in the block).
  template <class T>
  void bindArray(Module& m, std::string_view name, const std::vector<T>& v) {
    DeviceBuffer b = allocate(v.size() * sizeof(T));
    if (!v.empty()) check(cu_.cuMemcpyHtoD(b.ptr, v.data(), v.size() * sizeof(T)), "cuMemcpyHtoD");
    setBuffer(m, name, b.ptr, v.size());
    scene_buffers_.push_back(b);
  }
  // As bindArray, in pinned RAM when `inRam` (textures and the environment at memory tiers 1 and 2).
  template <class T>
  void bindMovable(Module& m, std::string_view name, const std::vector<T>& v, bool inRam) {
    if (!inRam) return bindArray(m, name, v);
    DeviceBuffer b = allocateHost(v.size() * sizeof(T));
    if (!v.empty()) std::memcpy(b.host, v.data(), v.size() * sizeof(T));
    setBuffer(m, name, b.ptr, v.size());
    scene_buffers_.push_back(b);
  }
  void setBuffer(Module& m, std::string_view name, CUdeviceptr ptr, size_t count);
  void setPointer(Module& m, std::string_view name, CUdeviceptr ptr);
  void writeGlobals(Module& m);
  void releaseScene();
  void ensureOutput(uint64_t pixels);

  const Driver& cu_;
  CUdevice device_ = 0;
  CUcontext ctx_ = nullptr;
  DeviceInfo info_;
  Module render_;
  std::optional<GpuScene> pending_;  // flattened by measure(), uploaded by upload()
  std::optional<GpuBvh> pending_bvh_;
  std::vector<DeviceBuffer> scene_buffers_;
  DeviceBuffer frame_buffer_, out_buffer_, counter_buffer_;
  uint64_t out_capacity_ = 0;
  bool scene_ready_ = false;
  const Scene* scene_ = nullptr;
  std::atomic<bool> cancel_{false};
  bool lost_ = false;  // the GPU stopped answering: nothing is waited for or freed any more (D-094)
  bool stack_warned_ = false;
};

const L::Global* findGlobal(std::string_view name) {
  for (const L::Global& g : L::kGlobals)
    if (g.name == name) return &g;
  return nullptr;
}

CudaBackend::CudaBackend(uint32_t ordinal) : cu_(driverState().cu) {
  const CudaProbe& probe = driverState().probe;
  if (!probe.driver_found) throw BackendError(fmt::format("CUDA: {}", probe.error));
  if (ordinal >= probe.gpus.size()) throw BackendError(fmt::format("there is no CUDA GPU {}", ordinal));
  const CudaGpu& gpu = probe.gpus[ordinal];
  info_.name = gpu.name;
  if (!gpu.usable) throw BackendError(fmt::format("{} cannot run this build's CUDA kernels: {}", gpu.name, gpu.why));
  check(cu_.cuDeviceGet(&device_, static_cast<int>(ordinal)), "cuDeviceGet");
  check(cu_.cuDevicePrimaryCtxRetain(&ctx_, device_), "cuDevicePrimaryCtxRetain");
  info_.backend = BackendKind::Cuda;
  info_.threads = 0;
  info_.memory_bytes = gpu.vram_bytes;
  info_.hardware_rt = false;
  info_.uuid = gpu.uuid;
  CtxScope scope(cu_, ctx_);
  render_ = loadModule(kPathtraceFatbin, "render", L::kGlobalsSize);
  frame_buffer_ = allocate(sizeof(gpu::Frame));
  counter_buffer_ = allocate(16);
  setPointer(render_, "g_frame", frame_buffer_.ptr);
  setBuffer(render_, "g_counters", counter_buffer_.ptr, 4);
}

CudaBackend::~CudaBackend() {
  if (lost_) return;  // its kernels may still be running: leak them rather than hang or free memory in use
  {
    CtxScope scope(cu_, ctx_);
    cu_.cuCtxSynchronize();
    releaseScene();
    release(frame_buffer_);
    release(out_buffer_);
    release(counter_buffer_);
    unloadModule(render_);
  }
  if (ctx_) cu_.cuDevicePrimaryCtxRelease(device_);
}

Module CudaBackend::loadModule(const unsigned char* image, const char* kernel, uint32_t globalsSize) {
  Module m;
  char log[8192] = {};
  CUjit_option opts[] = {CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES};
  void* vals[] = {log, reinterpret_cast<void*>(static_cast<uintptr_t>(sizeof(log)))};
  const CUresult r = cu_.cuModuleLoadDataEx(&m.module, image, 2, opts, vals);
  if (r != CUDA_SUCCESS)
    throw BackendError(fmt::format("CUDA on {}: cannot load the kernels: {}{}{}", info_.name, errorText(cu_, r),
                                   log[0] ? "\n" : "", log));
  check(cu_.cuModuleGetFunction(&m.function, m.module, kernel), "cuModuleGetFunction");
  check(cu_.cuModuleGetGlobal(&m.globals, &m.globals_size, m.module, "SLANG_globalParams"), "cuModuleGetGlobal");
  if (m.globals_size < globalsSize)
    throw BackendError(fmt::format("CUDA: the {} kernel's parameter block is {} bytes, the build expected {}", kernel,
                                   m.globals_size, globalsSize));
  m.host.assign(m.globals_size, 0);
  return m;
}

void CudaBackend::unloadModule(Module& m) {
  if (m.module) cu_.cuModuleUnload(m.module);
  m = Module{};
}

DeviceBuffer CudaBackend::allocate(size_t bytes) {
  DeviceBuffer b;
  b.bytes = std::max<size_t>(bytes, 16);  // never null, so empty arrays still have a valid pointer
  const CUresult r = cu_.cuMemAlloc(&b.ptr, b.bytes);
  if (r == CUDA_ERROR_OUT_OF_MEMORY)
    throw OutOfMemoryError(fmt::format("CUDA: cannot allocate {} of VRAM on {}: {}", formatBytes(b.bytes),
                                       info_.name, errorText(cu_, r)));
  if (r != CUDA_SUCCESS)
    throw BackendError(fmt::format("CUDA: cannot allocate {} on {}: {}", formatBytes(b.bytes), info_.name,
                                   errorText(cu_, r)));
  return b;
}

DeviceBuffer CudaBackend::allocateHost(size_t bytes) {
  DeviceBuffer b;
  b.bytes = std::max<size_t>(bytes, 16);
  CUresult r = cu_.cuMemHostAlloc(&b.host, b.bytes, CU_MEMHOSTALLOC_DEVICEMAP);
  if (r == CUDA_SUCCESS) {
    r = cu_.cuMemHostGetDevicePointer(&b.ptr, b.host, 0);
    if (r != CUDA_SUCCESS) cu_.cuMemFreeHost(b.host);
  }
  if (r != CUDA_SUCCESS)
    throw OutOfMemoryError(fmt::format("CUDA: cannot pin {} of RAM for {}: {}", formatBytes(b.bytes), info_.name,
                                       errorText(cu_, r)));
  return b;
}

void CudaBackend::release(DeviceBuffer& b) {
  if (b.host) cu_.cuMemFreeHost(b.host);
  else if (b.ptr) cu_.cuMemFree(b.ptr);
  b = DeviceBuffer{};
}

void CudaBackend::setBuffer(Module& m, std::string_view name, CUdeviceptr ptr, size_t count) {
  // The parity module has its own layout; both are looked up by name.
  uint32_t offset = kMissing, size = 0;
  if (&m == &render_) {
    if (const L::Global* g = findGlobal(name)) offset = g->offset, size = g->size;
  } else {
    for (const auto& g : cuda_layout::parity::kGlobals)
      if (g.name == name) offset = g.offset, size = g.size;
  }
  if (offset == kMissing || size != 16 || offset + 16 > m.host.size())
    throw BackendError(fmt::format("CUDA: the kernel has no buffer parameter {}", name));
  const uint64_t p = ptr, n = count;
  std::memcpy(m.host.data() + offset, &p, 8);
  std::memcpy(m.host.data() + offset + 8, &n, 8);
}

void CudaBackend::setPointer(Module& m, std::string_view name, CUdeviceptr ptr) {
  uint32_t offset = kMissing, size = 0;
  if (&m == &render_) {
    if (const L::Global* g = findGlobal(name)) offset = g->offset, size = g->size;
  } else {
    for (const auto& g : cuda_layout::parity::kGlobals)
      if (g.name == name) offset = g.offset, size = g.size;
  }
  if (offset == kMissing || size != 8 || offset + 8 > m.host.size())
    throw BackendError(fmt::format("CUDA: the kernel has no constant buffer parameter {}", name));
  const uint64_t p = ptr;
  std::memcpy(m.host.data() + offset, &p, 8);
}

void CudaBackend::writeGlobals(Module& m) {
  check(cu_.cuMemcpyHtoD(m.globals, m.host.data(), m.host.size()), "cuMemcpyHtoD (parameters)");
}

// ---------------------------------------------------------------- scene

uint64_t CudaBackend::vramBudget() {
  // Free VRAM now (other programs' use included), less the 10% margin (docs/GUIDE.md "Budget").
  CtxScope scope(cu_, ctx_);
  size_t free = 0, total = 0;
  check(cu_.cuMemGetInfo(&free, &total), "cuMemGetInfo");
  return uint64_t(free) / 10 * 9;
}

SceneFootprint CudaBackend::measure(const Scene& scene) {
  // The BVH is built on the CPU, so its exact size is known before anything goes to the GPU.
  pending_ = flattenScene(scene);
  pending_bvh_ = buildGpuBvh(scene);
  const GpuScene& g = *pending_;
  const GpuBvh& bvh = *pending_bvh_;
  const auto bytes = [](const auto& v) { return std::max<uint64_t>(16, v.size() * sizeof(v[0])); };
  SceneFootprint fp;
  fp.fixed = bytes(g.instances) + bytes(g.primitives) + bytes(g.positions) + bytes(g.normals) + bytes(g.tangents) +
             bytes(g.uvs) + bytes(g.colors) + bytes(g.indices) + bytes(g.materials) + bytes(g.textures) +
             bytes(g.emissive) + bytes(g.emissive_offset) + bytes(g.light_table) + bytes(g.lights) +
             bytes(g.env_tables) + bytes(g.sheen_table) + bytes(g.srgb_table) + bytes(bvh.nodes) + bytes(bvh.tris) +
             bytes(bvh.blas) + bytes(bvh.tlas) + bytes(bvh.tlas_index) + bytes(bvh.tri_map);
  fp.environment = bytes(*g.env_map) + bytes(*g.env_weights);
  for (const Texture& t : scene.textures) fp.textures.push_back({t.width, t.height, t.is_float ? 16u : 4u});
  return fp;
}

void CudaBackend::releaseScene() {
  for (auto& b : scene_buffers_) release(b);
  scene_buffers_.clear();
  scene_ready_ = false;
}

void CudaBackend::upload(const Scene& scene, const Placement& placement) {
  const auto t0 = std::chrono::steady_clock::now();
  CtxScope scope(cu_, ctx_);
  check(cu_.cuCtxSynchronize(), "cuCtxSynchronize");
  releaseScene();

  if (!pending_) pending_ = flattenScene(scene);
  if (!pending_bvh_) pending_bvh_ = buildGpuBvh(scene);
  GpuScene g = std::move(*pending_);
  const GpuBvh bvh = std::move(*pending_bvh_);
  pending_.reset();
  pending_bvh_.reset();
  if (!placement.halvings.empty()) packTextures(scene, placement.halvings, g);  // tier 2
  const bool inRam = placement.movable_in_ram;                                  // tiers 1 and 2
  const double bvhSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  for (const std::string& n : bvh.notes) spdlog::info("{}: {}", info_.name, n);

  bindArray(render_, "g_instances", g.instances);
  bindArray(render_, "g_primitives", g.primitives);
  bindArray(render_, "g_positions", g.positions);
  bindArray(render_, "g_normals", g.normals);
  bindArray(render_, "g_tangents", g.tangents);
  bindArray(render_, "g_uvs", g.uvs);
  bindArray(render_, "g_colors", g.colors);
  bindArray(render_, "g_indices", g.indices);
  bindArray(render_, "g_materials", g.materials);
  bindArray(render_, "g_textures", g.textures);
  for (size_t p = 0; p < kTexelParts; ++p) {  // D-109
    const std::string suffix = p == 0 ? "" : fmt::format("_{}", p);
    bindMovable(render_, "g_texels8" + suffix, g.texels8[p], inRam);
    bindMovable(render_, "g_texels32" + suffix, g.texels32[p], inRam);
  }
  bindArray(render_, "g_emissive", g.emissive);
  bindArray(render_, "g_emissive_offset", g.emissive_offset);
  bindArray(render_, "g_light_table", g.light_table);
  bindArray(render_, "g_lights", g.lights);
  bindMovable(render_, "g_env_map", *g.env_map, inRam);
  bindArray(render_, "g_env_rows", g.env_tables);
  bindMovable(render_, "g_env_weights", *g.env_weights, inRam);
  bindArray(render_, "g_sheen_table", g.sheen_table);
  bindArray(render_, "g_srgb_lut", g.srgb_table);
  bindArray(render_, "g_bvh_nodes", bvh.nodes);
  bindArray(render_, "g_bvh_tris", bvh.tris);
  bindArray(render_, "g_blas", bvh.blas);
  bindArray(render_, "g_tlas_nodes", bvh.tlas);
  bindArray(render_, "g_tlas_index", bvh.tlas_index);
  {
    // uint2 per triangle id.
    DeviceBuffer b = allocate(bvh.tri_map.size() * sizeof(uint32_t));
    if (!bvh.tri_map.empty())
      check(cu_.cuMemcpyHtoD(b.ptr, bvh.tri_map.data(), bvh.tri_map.size() * sizeof(uint32_t)), "cuMemcpyHtoD");
    setBuffer(render_, "g_tri_map", b.ptr, bvh.tri_map.size() / 2);
    scene_buffers_.push_back(b);
  }
  scene_ = &scene;
  scene_ready_ = true;
  size_t bvhBytes = (bvh.nodes.size() + bvh.tris.size()) * sizeof(Vec4f) + bvh.tlas.size() * sizeof(gpu::TlasNode);
  spdlog::info("{}: scene uploaded in {:.2f}s (BVH {} built in {:.2f}s)", info_.name,
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), formatBytes(bvhBytes),
               bvhSeconds);
}

// ---------------------------------------------------------------- frames and items

void CudaBackend::prepare(const FrameSetup& frame) {
  cancel_ = false;
  stack_warned_ = false;
  if (!scene_ready_) throw BackendError("CUDA: prepare() before upload()");
  const gpu::Frame f = makeGpuFrame(frame, *scene_);
  CtxScope scope(cu_, ctx_);
  check(cu_.cuMemcpyHtoD(frame_buffer_.ptr, &f, sizeof(f)), "cuMemcpyHtoD (frame)");
  ensureOutput(256 * 256);
}

void CudaBackend::ensureOutput(uint64_t pixels) {
  if (pixels <= out_capacity_ && out_buffer_.ptr) return;
  release(out_buffer_);
  out_buffer_ = allocate(pixels * sizeof(Vec3f));
  out_capacity_ = pixels;
}

void CudaBackend::render(const WorkItem& item, TileResult& out) {
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
  CtxScope scope(cu_, ctx_);
  ensureOutput(r.pixels());
  setBuffer(render_, "g_out", out_buffer_.ptr, r.pixels());
  writeGlobals(render_);
  check(cu_.cuMemsetD8(counter_buffer_.ptr, 0, 16), "cuMemsetD8");

  // Launches of at most ~1M paths, so no single launch runs long (display watchdogs, cancel); the running sum
  // stays in the output buffer, so the result is the same bits as one launch (gpu::kItemAccumulate).
  constexpr uint64_t kPathsPerLaunch = uint64_t(1) << 20;
  const uint32_t total = item.sample_end - item.sample_begin;
  if (total == 0) return;
  const uint32_t step =
      static_cast<uint32_t>(std::clamp<uint64_t>(kPathsPerLaunch / std::max<uint64_t>(r.pixels(), 1), 1, total));
  constexpr unsigned kBlockX = 16, kBlockY = 8;  // shaders/pathtrace_cuda.slang numthreads
  for (uint32_t s = item.sample_begin; s < item.sample_end;) {
    const uint32_t e = std::min(item.sample_end, s + step);
    gpu::PushConstants pc{r.x0, r.y0, r.width(), r.height(), s, e, total, 0};
    if (s > item.sample_begin) pc.flags |= gpu::kItemAccumulate;
    if (e == item.sample_end) pc.flags |= gpu::kItemFinish;
    void* args[] = {&pc};
    check(cu_.cuLaunchKernel(render_.function, (r.width() + kBlockX - 1) / kBlockX,
                             (r.height() + kBlockY - 1) / kBlockY, 1, kBlockX, kBlockY, 1, 0, nullptr, args, nullptr),
          "cuLaunchKernel");
    s = e;
    if (s < item.sample_end && cancel_) {
      out.cancelled = true;
      synchronize("rendering");
      return;
    }
  }
  synchronize("rendering");
  check(cu_.cuMemcpyDtoH(out.mean.data(), out_buffer_.ptr, r.pixels() * sizeof(Vec3f)), "cuMemcpyDtoH");
  uint32_t counters[4] = {};
  check(cu_.cuMemcpyDtoH(counters, counter_buffer_.ptr, sizeof(counters)), "cuMemcpyDtoH");
  out.nan_count = counters[0];
  if (counters[1] > 0 && !stack_warned_) {
    stack_warned_ = true;
    spdlog::warn("{}: {} BVH traversals hit the stack limit in work item {} and may have missed geometry", info_.name,
                 counters[1], item.id);
  }
  out.cancelled = cancel_.load();
  out.render_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// Waits for the launches so far by polling, so a GPU that stops answering cannot hold its slot (and the end of the
// render) forever: once cancelled, the kernels get 10 s to finish, then the device counts as lost (D-094). A
// launch error (illegal address and other sticky errors) throws here; every later call fails the same way.
void CudaBackend::synchronize(const char* what) {
  constexpr auto kHungGrace = std::chrono::seconds(10);
  std::chrono::steady_clock::time_point cancelledAt{};
  for (;;) {
    const CUresult r = cu_.cuStreamQuery(nullptr);
    if (r == CUDA_SUCCESS) return;
    if (r != CUDA_ERROR_NOT_READY) check(r, what);
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    if (!cancel_) continue;
    const auto now = std::chrono::steady_clock::now();
    if (cancelledAt == std::chrono::steady_clock::time_point{}) cancelledAt = now;
    if (now - cancelledAt > kHungGrace) {
      lost_ = true;
      throw BackendError(fmt::format("CUDA: {} does not answer: its kernels still run 10 s after the cancel",
                                     info_.name));
    }
  }
}

// ---------------------------------------------------------------- parity test

std::vector<float> CudaBackend::runParity(uint32_t count) {
  constexpr uint32_t kValues = 32;
  CtxScope scope(cu_, ctx_);
  Module m = loadModule(kParityFatbin, "parity", cuda_layout::parity::kGlobalsSize);
  const std::vector<float> sheen(sheenAlbedoTable(), sheenAlbedoTable() + kSheenTableSize * kSheenTableSize);
  DeviceBuffer sheenBuf = allocate(sheen.size() * sizeof(float));
  check(cu_.cuMemcpyHtoD(sheenBuf.ptr, sheen.data(), sheen.size() * sizeof(float)), "cuMemcpyHtoD");
  DeviceBuffer outBuf = allocate(size_t(count) * kValues * sizeof(float));
  DeviceBuffer push = allocate(16);
  check(cu_.cuMemcpyHtoD(push.ptr, &count, sizeof(count)), "cuMemcpyHtoD");
  setBuffer(m, "g_sheen_table", sheenBuf.ptr, sheen.size());
  setBuffer(m, "g_parity_out", outBuf.ptr, size_t(count) * kValues);
  setPointer(m, "g_parity", push.ptr);
  writeGlobals(m);
  check(cu_.cuLaunchKernel(m.function, (count + 63) / 64, 1, 1, 64, 1, 1, 0, nullptr, nullptr, nullptr),
        "cuLaunchKernel");
  check(cu_.cuCtxSynchronize(), "the parity kernel");
  std::vector<float> result(size_t(count) * kValues);
  check(cu_.cuMemcpyDtoH(result.data(), outBuf.ptr, result.size() * sizeof(float)), "cuMemcpyDtoH");
  release(push);
  release(outBuf);
  release(sheenBuf);
  unloadModule(m);
  return result;
}

}  // namespace

const CudaProbe& probeCuda() { return driverState().probe; }

std::string cudaBuildSummary() {
  std::string sass;
  for (int a : kSass) sass += fmt::format(" sm_{}", a);
  return fmt::format("CUDA {}.{} kernels:{}, PTX compute_{}", kToolkitVersion / 1000, kToolkitVersion % 1000 / 10, sass,
                     kPtxArch);
}

std::unique_ptr<IBackend> createCudaBackend(uint32_t ordinal) { return std::make_unique<CudaBackend>(ordinal); }

std::vector<float> runCudaParity(uint32_t ordinal, uint32_t count) {
  CudaBackend b(ordinal);
  return b.runParity(count);
}

}  // namespace pen
