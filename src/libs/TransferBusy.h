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
// diagnose request, a heartbeat, or a frame the machine never answers (a
// relay, a presence reply, a job-start ready). A frame whose type or text is
// cut off by the end of `data` counts as needing a reply, since it cannot be
// ruled out.
bool needs_busy_reply(const uint8_t* data, std::size_t length);

// Decides which discarded reads get a busy reply during one transfer. The
// first read from a client that is not sent transfer notices (below) always
// does, whatever it holds, so it learns once why the machine has gone quiet.
// A client that is sent them learns that from the notices, so it is only
// answered for a read that needs_busy_reply(): a controller waiting for the
// answer to its own command can then take a busy reply as that answer. After
// the first reply, a read that needs_busy_reply() gets one, no more often
// than busy_reply_gap_us, and status queries and heartbeats get nothing
// more. clear() starts over for the next transfer.
class BusyReplyLimiter {
 public:
  bool should_reply(const Address& sender, bool needs_reply, bool sent_transfer_notices, uint32_t now_us);
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

// While a file transfer runs, the machine does nothing else: no status, no
// replies, nothing read from USB. So that the other connected controllers
// do not take that silence for a lost link, and the WiFi module does not
// close a connection that carries no traffic, every identified controller
// except the one transferring is sent a transfer notice once a second: a
// 0x68 event, kind event_kind_transfer (libs/Publish.h), whose one byte of
// data after the kind says which way the file is going.
constexpr uint8_t transfer_download = 0;  // a controller is reading a file from the machine
constexpr uint8_t transfer_upload = 1;    // a controller is sending a file to the machine
constexpr uint32_t transfer_notice_interval_us = 1000000;
constexpr std::size_t transfer_notice_frame_size = 2 + makera::frame_overhead;

// Writes the transfer notice for `direction` into `out` as one whole frame.
// Returns its length, or 0 if `capacity` is too small.
std::size_t build_transfer_notice_frame(uint8_t direction, uint8_t* out, std::size_t capacity);

// When the next transfer notice is due. begin() makes the first one due at
// once; after that, due() says yes once every transfer_notice_interval_us.
// Both take raw us_ticker_read() readings.
class TransferNotice {
 public:
  void begin(uint8_t direction, uint32_t now_us) {
    direction_ = direction;
    last_us_ = now_us - transfer_notice_interval_us;
  }
  // True if a notice is due at `now_us`, which then counts as sent.
  bool due(uint32_t now_us) {
    if (static_cast<uint32_t>(now_us - last_us_) < transfer_notice_interval_us) return false;
    last_us_ = now_us;
    return true;
  }
  uint8_t direction() const { return direction_; }

 private:
  uint8_t direction_ = transfer_download;
  uint32_t last_us_ = 0;
};

}  // namespace multiclient
