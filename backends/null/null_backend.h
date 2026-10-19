#pragma once

#include <memory>

#include "backends/backend.h"

namespace pen {

// Test-only backend (`--backend null`, hidden; DECISIONS D-047): returns correctly shaped results instantly.
// Pixel values are a hash of (x, y, sample range), so the order in which chunks are merged changes the bits and
// the scheduler's determinism is really tested. Not a renderer: the images are noise.
std::unique_ptr<IBackend> createNullBackend(uint32_t index);

}  // namespace pen
