#include "controller/local_worker.h"

#include <chrono>

namespace pen {

LocalWorker::LocalWorker(WorkerId id, WorkerEvents& events, std::vector<std::unique_ptr<IBackend>> devices)
    : id_(id), events_(events), devices_(std::move(devices)) {}

LocalWorker::~LocalWorker() { complete(); }

WorkerInfo LocalWorker::info() const {
  WorkerInfo w;
  w.name = "local";
  w.local = true;
  for (const auto& d : devices_) w.devices.push_back(d->info());
  return w;
}

void LocalWorker::offerScene(std::shared_ptr<const Scene> scene, const RenderSettings& settings) {
  scene_ = std::move(scene);
  std::vector<std::string> notes;  // the controller logs camera notes itself
  FrameSetup frame;
  frame.camera = RenderCamera(*scene_, settings, settings.outputWidth(), settings.outputHeight(), notes);
  frame.params = IntegratorParams::from(settings, *scene_);
  frame.seed = settings.seed;

  std::vector<DeviceReady> ready;
  const SceneStats stats = scene_->stats();
  for (DeviceId d = 0; d < devices_.size(); ++d) {
    const auto start = std::chrono::steady_clock::now();
    const MemoryPlan plan = devices_[d]->plan(stats);
    devices_[d]->upload(*scene_, plan);
    devices_[d]->prepare(frame);
    DeviceReady r;
    r.device = d;
    r.backend = devices_[d]->info().backend;
    r.memory_tier = plan.tier;
    r.summary = plan.summary;
    r.build_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ready.push_back(r);
  }
  events_.onReady(id_, ready);

  for (DeviceId d = 0; d < devices_.size(); ++d) {
    DeviceSlot::Callbacks cb;
    cb.request_work = [this](uint32_t device, int count) { events_.onRequestWork(id_, device, count); };
    cb.result = [this](TileResult&& r) { events_.onTileResult(id_, std::move(r)); };
    cb.failed = [this](uint32_t device, const std::string& error) {
      events_.onGone(id_, GoneReason::Failed, "device " + std::to_string(device) + ": " + error);
    };
    slots_.push_back(std::make_unique<DeviceSlot>(d, *devices_[d], cb));
  }
  for (auto& s : slots_) s->start();
}

void LocalWorker::sendLease(const Lease& lease) {
  if (lease.device < slots_.size()) slots_[lease.device]->addLease(lease.item);
}

void LocalWorker::cancel(std::span<const LeaseId>) {}  // M3: cancellation and requeue
void LocalWorker::ackTile(LeaseId, AckStatus) {}       // in-process results need no acknowledgement
void LocalWorker::pause(bool) {}                       // M2: pause on disk full

void LocalWorker::complete() {
  for (auto& s : slots_) s->finish();
  for (auto& s : slots_) s->join();
  slots_.clear();
}

}  // namespace pen
