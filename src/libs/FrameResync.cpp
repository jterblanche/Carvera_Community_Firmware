#include "FrameResync.h"

namespace makera {

ResyncResult ResyncingDecoder::decode_byte(uint8_t byte, uint32_t now_us) {
  const DecodeResult result = decoder_.decode_byte(byte, now_us);

  if (result == DecodeResult::incomplete) {
    // Once in_progress() (a header byte matched, or we're past the header
    // entirely), this byte is real progress, not a failure -- see the
    // class comment for why this is in_progress(), not has_header().
    if (decoder_.in_progress()) return ResyncResult::incomplete;

    if (++header_errors_ >= max_header_errors) {
      decoder_.reset();
      header_errors_ = 0;
      if (!notified_once_) {
        notify_pending_ = true;
        notified_once_ = true;
      }
      return ResyncResult::header_error;
    }
    return ResyncResult::incomplete;
  }

  // Any non-incomplete result -- a completed frame, or the decoder giving
  // up on this one and restarting on its own (invalid_length/_crc/_footer)
  // -- means real progress happened, so the header-search failure count
  // starts over. This matches the pre-existing behaviour: the count tracks
  // a run of failures, not a lifetime total.
  header_errors_ = 0;

  switch (result) {
    case DecodeResult::complete: return ResyncResult::complete;
    case DecodeResult::invalid_length: return ResyncResult::invalid_length;
    case DecodeResult::invalid_crc: return ResyncResult::invalid_crc;
    case DecodeResult::invalid_footer: return ResyncResult::invalid_footer;
    case DecodeResult::incomplete: break;  // unreachable, handled above
  }
  return ResyncResult::incomplete;
}

void ResyncingDecoder::reset() {
  decoder_.reset();
  header_errors_ = 0;
  notify_pending_ = false;
  notified_once_ = false;
}

bool ResyncingDecoder::consume_notify_pending() {
  const bool pending = notify_pending_;
  notify_pending_ = false;
  return pending;
}

}  // namespace makera
