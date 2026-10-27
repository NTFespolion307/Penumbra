#include "core/system/test_hooks.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>

namespace pen {

double testTimeScale() {
  static const double scale = [] {
    const char* v = std::getenv("PENUMBRA_TEST_TIME_SCALE");
    if (!v || !*v) return 1.0;
    const double s = std::clamp(std::strtod(v, nullptr), 0.01, 1.0);
    spdlog::warn("TEST HOOK ACTIVE (PENUMBRA_TEST_TIME_SCALE): network and fault timers x {}", s);
    return s;
  }();
  return scale;
}

}  // namespace pen
