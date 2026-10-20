#include "backends/cpu/cpu_backend.h"

#include <embree4/rtcore.h>
#include <tbb/blocked_range2d.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <fmt/format.h>
#include <limits>
#include <spdlog/spdlog.h>
#include <thread>

#include "core/system/sysinfo.h"

namespace pen {

namespace {

// User data for alpha-tested geometries.
struct AlphaGeometry {
  const Scene* scene;
  uint32_t primitive;
};

void alphaFilter(const RTCFilterFunctionNArguments* args) {
  const auto* g = static_cast<const AlphaGeometry*>(args->geometryUserPtr);
  for (unsigned i = 0; i < args->N; ++i) {
    if (args->valid[i] == 0) continue;
    const unsigned inst = RTCHitN_instID(args->hit, args->N, i, 0);
    const unsigned prim = RTCHitN_primID(args->hit, args->N, i);
    const float u = RTCHitN_u(args->hit, args->N, i), v = RTCHitN_v(args->hit, args->N, i);
    const Material& mat = g->scene->materials[g->scene->meshes[g->scene->instances[inst].mesh].primitives[g->primitive].material];
    const float alpha = alphaAt(*g->scene, inst, g->primitive, prim, u, v);
    // MASK: alpha cutoff. BLEND: treated as a 0.5 cutout (DECISIONS D-024).
    const float cutoff = mat.alpha_mode == AlphaMode::Mask ? mat.alpha_cutoff : 0.5f;
    if (alpha < cutoff) args->valid[i] = 0;
  }
}

class EmbreeTracer {
 public:
  explicit EmbreeTracer(RTCScene scene) : scene_(scene) {}

  Hit intersect(Vec3f o, Vec3f d, float tmax) const {
    RTCRayHit rh{};
    rh.ray.org_x = o.x;
    rh.ray.org_y = o.y;
    rh.ray.org_z = o.z;
    rh.ray.dir_x = d.x;
    rh.ray.dir_y = d.y;
    rh.ray.dir_z = d.z;
    rh.ray.tnear = 0;
    rh.ray.tfar = tmax;
    rh.ray.mask = 0xffffffffu;
    rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rh.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
    rtcIntersect1(scene_, &rh);
    Hit h;
    if (rh.hit.geomID == RTC_INVALID_GEOMETRY_ID) return h;
    h.valid = true;
    h.t = rh.ray.tfar;
    h.instance = rh.hit.instID[0];
    h.primitive = rh.hit.geomID;
    h.triangle = rh.hit.primID;
    h.u = rh.hit.u;
    h.v = rh.hit.v;
    return h;
  }

  bool occluded(Vec3f o, Vec3f d, float tmax) const {
    RTCRay r{};
    r.org_x = o.x;
    r.org_y = o.y;
    r.org_z = o.z;
    r.dir_x = d.x;
    r.dir_y = d.y;
    r.dir_z = d.z;
    r.tnear = 0;
    r.tfar = tmax;
    r.mask = 0xffffffffu;
    rtcOccluded1(scene_, &r);
    return r.tfar == -std::numeric_limits<float>::infinity();
  }

 private:
  RTCScene scene_;
};

void embreeError(void*, RTCError code, const char* message) {
  spdlog::error("Embree error {}: {}", static_cast<int>(code), message ? message : "");
}

class CpuBackend final : public IBackend {
 public:
  explicit CpuBackend(uint32_t threads) {
    const uint32_t hw = usableCpuThreads();
    threads_ = threads == 0 ? hw : threads;
    arena_ = std::make_unique<tbb::task_arena>(static_cast<int>(threads_));
    device_ = rtcNewDevice(fmt::format("threads={},set_affinity=0", threads_).c_str());
    if (!device_) throw BackendError("cannot create the Embree device");
    rtcSetDeviceErrorFunction(device_, embreeError, nullptr);
    const SystemInfo sys = querySystem();
    info_.name = sys.cpu;
    info_.backend = BackendKind::Cpu;
    info_.threads = threads_;
    info_.memory_bytes = sys.ram_total;
  }

  ~CpuBackend() override {
    if (top_) rtcReleaseScene(top_);
    for (RTCScene s : blas_) rtcReleaseScene(s);
    if (device_) rtcReleaseDevice(device_);
  }

  DeviceInfo info() const override { return info_; }

  MemoryPlan plan(const SceneStats& stats) override {
    MemoryPlan p;
    p.tier = 0;
    p.summary = fmt::format("tier 0, scene {:.1f} MB in RAM", (stats.geometry_bytes + stats.texture_bytes) / 1048576.0);
    return p;
  }

  void upload(const Scene& scene, const MemoryPlan&) override {
    scene_ = &scene;
    arena_->execute([&] { build(scene); });
  }

  void prepare(const FrameSetup& frame) override {
    frame_ = frame;
    cancel_ = false;
  }

  void cancel() override { cancel_ = true; }

  void render(const WorkItem& item, TileResult& out) override {
    const auto start = std::chrono::steady_clock::now();
    const TileRect r = item.rect;
    out.item_id = item.id;
    out.rect = r;
    out.sample_begin = item.sample_begin;
    out.sample_end = item.sample_end;
    out.mean.assign(r.pixels(), Vec3f(0));
    std::atomic<uint32_t> nans{0};
    const EmbreeTracer tracer(top_);
    const Scene& scene = *scene_;
    const FrameSetup& f = frame_;
    const float invN = item.samples() > 0 ? 1.0f / static_cast<float>(item.samples()) : 0.0f;

    arena_->execute([&] {
      tbb::parallel_for(tbb::blocked_range2d<uint32_t>(r.y0, r.y1, 4, r.x0, r.x1, 16), [&](const auto& range) {
        uint32_t localNans = 0;
        for (uint32_t y = range.rows().begin(); y < range.rows().end(); ++y) {
          if (cancel_.load(std::memory_order_relaxed)) return;
          for (uint32_t x = range.cols().begin(); x < range.cols().end(); ++x) {
            Vec3f sum(0);
            for (uint32_t s = item.sample_begin; s < item.sample_end; ++s) {
              Sampler sampler(x, y, s, f.seed);
              const Vec2f jitter = sampler.next2D();
              const Vec2f lens = sampler.next2D();
              Vec3f o, d;
              f.camera.generateRay(x + jitter.x, y + jitter.y, lens, o, d);
              Vec3f L = traceSample(scene, tracer, f.params, o, d, sampler);
              if (!isFinite(L)) {
                ++localNans;
                L = Vec3f(0);
              }
              sum += L;
            }
            out.mean[size_t(y - r.y0) * r.width() + (x - r.x0)] = sum * invN;
          }
        }
        if (localNans) nans += localNans;
      });
    });
    out.nan_count = nans.load();
    out.cancelled = cancel_.load();
    out.render_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  }

 private:
  void build(const Scene& scene) {
    for (RTCScene s : blas_) rtcReleaseScene(s);
    blas_.clear();
    alpha_.clear();
    alpha_.reserve(1024);
    for (const Mesh& mesh : scene.meshes) {
      RTCScene blas = rtcNewScene(device_);
      rtcSetSceneFlags(blas, RTC_SCENE_FLAG_ROBUST);
      rtcSetSceneBuildQuality(blas, RTC_BUILD_QUALITY_HIGH);
      for (uint32_t p = 0; p < mesh.primitives.size(); ++p) {
        const MeshPrimitive& prim = mesh.primitives[p];
        RTCGeometry g = rtcNewGeometry(device_, RTC_GEOMETRY_TYPE_TRIANGLE);
        rtcSetGeometryBuildQuality(g, RTC_BUILD_QUALITY_HIGH);
        // Embree-owned buffers: they carry the padding Embree's SIMD loads need.
        auto* vb = static_cast<float*>(rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
                                                               3 * sizeof(float), prim.positions.size()));
        for (size_t i = 0; i < prim.positions.size(); ++i) {
          vb[i * 3] = prim.positions[i].x;
          vb[i * 3 + 1] = prim.positions[i].y;
          vb[i * 3 + 2] = prim.positions[i].z;
        }
        auto* ib = static_cast<uint32_t*>(rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
                                                                  3 * sizeof(uint32_t), prim.triangleCount()));
        std::copy(prim.indices.begin(), prim.indices.end(), ib);
        if (scene.materials[prim.material].usesAlpha()) {
          alpha_.push_back(std::make_unique<AlphaGeometry>(AlphaGeometry{&scene, p}));
          rtcSetGeometryUserData(g, alpha_.back().get());
          rtcSetGeometryIntersectFilterFunction(g, alphaFilter);
          rtcSetGeometryOccludedFilterFunction(g, alphaFilter);
        }
        rtcCommitGeometry(g);
        rtcAttachGeometryByID(blas, g, p);
        rtcReleaseGeometry(g);
      }
      rtcCommitScene(blas);
      blas_.push_back(blas);
    }
    if (top_) rtcReleaseScene(top_);
    top_ = rtcNewScene(device_);
    rtcSetSceneFlags(top_, RTC_SCENE_FLAG_ROBUST);
    rtcSetSceneBuildQuality(top_, RTC_BUILD_QUALITY_HIGH);
    for (uint32_t i = 0; i < scene.instances.size(); ++i) {
      RTCGeometry g = rtcNewGeometry(device_, RTC_GEOMETRY_TYPE_INSTANCE);
      rtcSetGeometryInstancedScene(g, blas_[scene.instances[i].mesh]);
      rtcSetGeometryTransform(g, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, &scene.instances[i].to_world.m[0][0]);
      rtcCommitGeometry(g);
      rtcAttachGeometryByID(top_, g, i);
      rtcReleaseGeometry(g);
    }
    rtcCommitScene(top_);
  }

  uint32_t threads_ = 1;
  std::atomic<bool> cancel_{false};
  std::unique_ptr<tbb::task_arena> arena_;
  RTCDevice device_ = nullptr;
  RTCScene top_ = nullptr;
  std::vector<RTCScene> blas_;
  std::vector<std::unique_ptr<AlphaGeometry>> alpha_;
  const Scene* scene_ = nullptr;
  FrameSetup frame_;
  DeviceInfo info_;
};

}  // namespace

std::unique_ptr<IBackend> createCpuBackend(uint32_t threads) { return std::make_unique<CpuBackend>(threads); }

}  // namespace pen
