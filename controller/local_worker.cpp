#include "controller/local_worker.h"

#include "worker/device_prep.h"

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

void LocalWorker::offerScene(const RenderOffer& offer) {
  scene_ = offer.scene;
  std::vector<DeviceReady> ready;
  std::vector<bool> skipped;
  for (const PreparedDevice& p : prepareDevices(*scene_, offer.settings, devices_)) {
    ready.push_back({p.device, p.backend, p.memory_tier, p.build_seconds, p.summary, p.skipped, p.reason});
    skipped.push_back(p.skipped);
  }
  events_.onReady(id_, ready);

  for (DeviceId d = 0; d < devices_.size(); ++d) {
    DeviceSlot::Callbacks cb;
    cb.request_work = [this](uint32_t device, int count) { events_.onRequestWork(id_, device, count); };
    cb.result = [this](TileResult&& r) { events_.onTileResult(id_, std::move(r)); };
    cb.failed = [this](uint32_t device, const std::string& error, const std::vector<LeaseId>& lost) {
      // The worker keeps going with its other devices (docs/GUIDE.md "Device failure").
      events_.onDeviceFailed(id_, device, error);
      events_.onRelease(id_, lost);
    };
    slots_.push_back(std::make_unique<DeviceSlot>(d, *devices_[d], cb));
  }
  // A skipped device keeps its slot (so device numbers stay) but never starts, so it never asks for work.
  for (DeviceId d = 0; d < slots_.size(); ++d)
    if (!skipped[d]) slots_[d]->start();
}

void LocalWorker::sendLease(const Lease& lease) {
  if (lease.device < slots_.size()) {
    WorkItem item = lease.item;
    item.lease = lease.id;
    slots_[lease.device]->addLease(item);
  }
}

// Cancels everything (Ctrl+C, out of disk, end of render); single leases are never cancelled yet.
void LocalWorker::cancel(std::span<const LeaseId>) {
  for (auto& s : slots_) s->clearQueue();
  for (auto& d : devices_) d->cancel();
}
void LocalWorker::ackTile(LeaseId, AckStatus) {}  // in-process results need no acknowledgement
void LocalWorker::pause(bool) {}                  // disk-full pause arrives with the UI (M10)

void LocalWorker::complete() {
  for (auto& s : slots_) s->finish();
  for (auto& s : slots_) s->join();
  slots_.clear();
}

}  // namespace pen
