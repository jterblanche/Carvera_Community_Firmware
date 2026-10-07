#pragma once

#include <cstdint>

// When the machine gives up on a framed-protocol download whose controller
// has stopped asking for data. A download runs inside the command, so the
// main loop does not run until it ends: no status, no other link's
// commands. A controller still in the download asks again at least every
// 5 seconds (it resends its request after 5 seconds with no reply), so 10
// seconds of silence means it has gone, with room for one lost request.
// Plain C++, no Kernel or mbed dependency, so it builds and runs on the
// host (see tests/TEST_TransferTimeout).
namespace transfer_timeout {

constexpr uint32_t download_stall_us = 10000000;

// True once more than download_stall_us have passed since the last request
// from the controller. Both arguments are raw us_ticker_read() readings;
// the unsigned difference is correct across one wrap of the counter.
constexpr bool download_stalled(uint32_t last_request_us, uint32_t now_us) {
  return static_cast<uint32_t>(now_us - last_request_us) > download_stall_us;
}

}  // namespace transfer_timeout
