#pragma once

#include <cstddef>
#include <cstdint>

#include "ClientTable.h"
#include "MakeraFrame.h"

namespace multiclient {

// While a WiFi file transfer runs, its receive loop reads every client's
// bytes but can only use the ones from the client that started it (see
// WifiProvider::gets()). Anything from another client is discarded, and its
// sender is told why with this reply, sent to it alone.
constexpr char busy_reply_text[] = "error:Busy -- a file transfer is in progress, retry when it finishes\r\n";
constexpr std::size_t busy_reply_frame_size = sizeof(busy_reply_text) - 1 + makera::frame_overhead;

// Shortest gap between two busy replies to the same client for frames that
// need one each time (see BusyReplyLimiter), so a client that keeps sending
// cannot fill the WiFi module's send buffer with replies.
constexpr uint32_t busy_reply_gap_us = 250000;

// Writes the busy reply into `out` as one text frame, the same type as any
// other command reply. Returns its length, or 0 if `capacity` is too small.
std::size_t build_busy_reply_frame(uint8_t* out, std::size_t capacity);

// True if `data`, one read from a client, holds any frame other than the
// traffic a controller sends on its own: a status query, a keep-alive, a
// diagnose request or a heartbeat. A frame whose type or text is cut off by
// the end of `data` counts as needing a reply, since it cannot be ruled out.
bool needs_busy_reply(const uint8_t* data, std::size_t length);

// Decides which discarded reads get a busy reply during one transfer. The
// first read from each client always does, whatever it holds, so every
// client learns once why the machine has gone quiet. After that, a read that
// needs_busy_reply() gets one, no more often than busy_reply_gap_us, and
// status queries and heartbeats get nothing more. clear() starts over for
// the next transfer.
class BusyReplyLimiter {
 public:
  bool should_reply(const Address& sender, bool needs_reply, uint32_t now_us);
  void clear() {
    count_ = 0;
    oldest_ = 0;
  }

 private:
  struct Entry {
    Address address;
    bool replied_to_frame = false;
    uint32_t last_frame_reply_us = 0;
  };

  // Enough for every other client the firmware keeps (max_wifi_clients
  // less the transfer's own) plus two connecting during the transfer. Past
  // that, the oldest entry is reused, which at worst sends one extra reply.
  static constexpr std::size_t capacity = max_wifi_clients + 1;
  Entry entries_[capacity];
  std::size_t count_ = 0;
  std::size_t oldest_ = 0;
};

}  // namespace multiclient
