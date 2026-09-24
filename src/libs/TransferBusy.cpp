#include "TransferBusy.h"

#include <cstring>

#include "CRC16.h"
#include "MakeraControl.h"
#include "PublicData.h"

namespace multiclient {

std::size_t build_busy_reply_frame(uint8_t* out, std::size_t capacity) {
  if (out == nullptr || capacity < busy_reply_frame_size) return 0;
  const std::size_t text_length = sizeof(busy_reply_text) - 1;
  const uint16_t length = static_cast<uint16_t>(text_length + 3);  // type, text, CRC
  makera::write_be16(out, makera::header);
  makera::write_be16(out + 2, length);
  out[4] = PTYPE_NORMAL_INFO;
  std::memcpy(out + 5, busy_reply_text, text_length);
  makera::write_be16(out + 5 + text_length, crc16::ccitt(out + 2, length));
  makera::write_be16(out + 7 + text_length, makera::footer);
  return busy_reply_frame_size;
}

bool needs_busy_reply(const uint8_t* data, std::size_t length) {
  if (data == nullptr) return false;
  std::size_t i = 0;
  while (i + 1 < length) {
    if (makera::read_be16(data + i) != makera::header) {
      ++i;
      continue;
    }
    if (i + 4 >= length) return true;
    const uint16_t frame_length = makera::read_be16(data + i + 2);  // type, data, CRC
    const uint8_t type = data[i + 4];
    if (type == PTYPE_CTRL_SINGLE) {
      if (i + 5 >= length) return true;
      const makera::ControlAction action = makera::decode_control(data[i + 5]);
      if (action != makera::ControlAction::query && action != makera::ControlAction::keep_alive &&
          action != makera::ControlAction::diagnose) {
        return true;
      }
    } else if (type == PTYPE_CTRL_MULTI) {
      if (frame_length < 3 || i + 5 + (frame_length - 3u) > length) return true;
      if (!makera::is_diagnostic_request(data + i + 5, frame_length - 3u)) return true;
    } else if (type != PTYPE_HEARTBEAT) {
      return true;
    }
    // Skip the whole frame, so its CRC cannot be mistaken for a header.
    i += 6 + static_cast<std::size_t>(frame_length);
  }
  return false;
}

bool BusyReplyLimiter::should_reply(const Address& sender, bool needs_reply, uint32_t now_us) {
  for (std::size_t k = 0; k < count_; ++k) {
    Entry& entry = entries_[k];
    if (!same_address(entry.address, sender)) continue;
    if (!needs_reply) return false;
    if (entry.replied_to_frame && static_cast<int32_t>(now_us - entry.last_frame_reply_us) < static_cast<int32_t>(busy_reply_gap_us)) {
      return false;
    }
    entry.replied_to_frame = true;
    entry.last_frame_reply_us = now_us;
    return true;
  }

  std::size_t slot;
  if (count_ < capacity) {
    slot = count_++;
  } else {
    slot = oldest_;
    oldest_ = (oldest_ + 1) % capacity;
  }
  Entry& entry = entries_[slot];
  entry.address = sender;
  entry.replied_to_frame = needs_reply;
  entry.last_frame_reply_us = now_us;
  return true;
}

}  // namespace multiclient
