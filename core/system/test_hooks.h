#pragma once

#include <chrono>

namespace pen {

// Test only (D-047 style, logged when active): PENUMBRA_TEST_TIME_SCALE=0.2 multiplies the network and fault
// timers (heartbeats, peer timeouts, reconnect backoff, handshake timeout) so the fault tests do not sit waiting
// on 30-second clocks (D-106). 1 when unset; clamped to 0.01-1.
double testTimeScale();

// `d` scaled by testTimeScale(), at least 1 ms.
template <class Rep, class Period>
std::chrono::milliseconds scaledForTests(std::chrono::duration<Rep, Period> d) {
  const double ms = std::chrono::duration<double, std::milli>(d).count() * testTimeScale();
  return std::chrono::milliseconds(ms < 1 ? 1 : static_cast<long long>(ms));
}

}  // namespace pen
