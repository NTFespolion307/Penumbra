#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "backends/backend.h"

namespace pen {

// Test-only fault injection (DECISIONS D-047): PENUMBRA_TEST_FAULTS=device=N,after=K,mode=throw|hang makes local
// device N fail (throw) or hang until cancelled, after it has rendered K items. Unset: nothing happens.
struct FaultSpec {
  uint32_t device = 0;
  uint32_t after = 0;
  bool hang = false;
};

// Reads PENUMBRA_TEST_FAULTS; throws std::invalid_argument on a malformed value.
std::optional<FaultSpec> faultSpecFromEnvironment();

std::unique_ptr<IBackend> wrapWithFault(std::unique_ptr<IBackend> backend, const FaultSpec& spec);

}  // namespace pen
