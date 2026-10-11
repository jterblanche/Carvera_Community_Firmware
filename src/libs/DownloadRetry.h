#pragma once

#include <cstdint>

#include "PublicData.h"

// How a framed-protocol download answers a retry. The controller asks for
// the file's MD5, then for its size (the view frame), then for each data
// packet by number. A retry asks for the last frame again and names no
// packet itself. Plain C++, no Kernel or mbed dependency, so it builds and
// runs on the host (see tests/TEST_DownloadRetry).
namespace download_retry {

// True if a retry received after sending `last_sent` repeats a data packet,
// so the next data reply sends the last packet again instead of the one its
// request names. Only a retry after a data packet does: one that arrives
// while the MD5 or view frame was the last sent re-sends that frame, and
// the data request that follows is served as it asks.
constexpr bool repeats_data_packet(uint8_t last_sent) {
  return last_sent == PTYPE_FILE_DATA;
}

}  // namespace download_retry
