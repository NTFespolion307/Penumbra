#pragma once

namespace pen::net {

// Seconds the network threads of this process have spent handling received data (decryption, frame parsing and the
// message handlers they call), for the controller's throughput summary (M9, D-112). No socket headers here, so the
// controller can read it (CLAUDE.md layering).
double networkBusySeconds();
// ... of which decrypting (TLS) what arrived.
double networkTlsSeconds();

}  // namespace pen::net
