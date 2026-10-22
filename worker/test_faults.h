#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "backends/backend.h"

namespace pen {

// Test-only fault injection (DECISIONS D-047, D-067): PENUMBRA_TEST_FAULTS=device=N,after=K,mode=M makes local
// device N misbehave once it has rendered K items. Modes:
//   throw    the device fails (BackendError)
//   hang     the device stops answering until cancelled
//   exit     the whole process exits at once with code 137 (a crashed worker)
//   freeze   the device hangs and a network worker stops sending anything, heartbeats included (a dead link)
//   corrupt  a network worker damages the device's tiles after computing their checksums
//   upload   the device fails while taking the scene (before any item; `after` is ignored), so it is skipped
// Unset: nothing happens.
enum class FaultMode { Throw, Hang, Exit, Freeze, Corrupt, Upload };

struct FaultSpec {
  uint32_t device = 0;
  uint32_t after = 0;
  FaultMode mode = FaultMode::Throw;
};

// Reads PENUMBRA_TEST_FAULTS; throws std::invalid_argument on a malformed value.
std::optional<FaultSpec> faultSpecFromEnvironment();
std::string_view toString(FaultMode mode);

std::unique_ptr<IBackend> wrapWithFault(std::unique_ptr<IBackend> backend, const FaultSpec& spec);

// Worker-level effects, set by a faulty device and acted on by the network worker.
bool testFrozen();
// True once per tile the faulty device rendered in corrupt mode.
bool testTakeCorruptTile();

}  // namespace pen
