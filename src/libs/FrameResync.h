#pragma once

#include "MakeraFrame.h"

namespace makera {

// A FrameDecoder mishandles a TCP stream that is not a clean run of frames:
// stray bytes before the first frame (a plain-text probe, a stale
// Smoothie-mode line, line noise) count as header-search failures, and
// FrameDecoder itself has no notion of giving up on a header hunt -- it
// just keeps hunting forever. WifiProvider needs a bound on that hunt (so a
// genuinely non-framed client gets told to update, once), and it needs that
// bound to never become permanent: once it fires, the very next byte must
// be tried again as a fresh header, indistinguishably from a client that
// had never sent anything bad at all. ResyncingDecoder wraps a FrameDecoder
// and owns exactly that bookkeeping, kept Kernel-free (like FrameDecoder
// itself) so it can be driven by a host test with the exact byte sequences
// a probe-then-frame connection produces -- see tests/TEST_FrameResync.
//
// A byte only counts against the threshold when it leaves the wrapped
// decoder completely blank -- FrameDecoder::in_progress() false, meaning
// not even the first header byte (0x86) matched. A byte that does match it
// is never blamed, even if the byte after turns out not to be the second
// header byte (0x68): only that following byte, having produced nothing,
// counts. This matters because FrameDecoder::has_header() only becomes
// true after BOTH header bytes are seen -- checking that instead of
// in_progress() would count the very first byte of every valid frame as a
// failure too, and a valid frame arriving exactly when the count is one
// short of the threshold would trip it on its own opening byte.
enum class ResyncResult : uint8_t {
  incomplete,
  complete,
  invalid_length,
  invalid_crc,
  invalid_footer,
  // A run of max_header_errors bytes never resolved into a header. The
  // decoder has already been resynchronised (reset, ready to try the very
  // next byte as a fresh header) before this is returned, so the caller
  // does not need to reset anything itself -- it only needs to decide
  // whether to show the diagnostic (see consume_notify_pending()) and then
  // keep feeding bytes.
  header_error,
};

class ResyncingDecoder {
 public:
  explicit ResyncingDecoder(Packet& packet) : decoder_(packet) {}

  // How many consecutive header-search failures (see the class comment)
  // are tolerated before resynchronising. Matches the pre-existing
  // threshold in WifiProvider::receive_wifi_data().
  static constexpr uint16_t max_header_errors = 20;

  // Feeds one byte through the wrapped decoder. Every result the decoder
  // itself can produce passes straight through, with one addition:
  // header_error, in place of what would otherwise have been the
  // max_header_errors-th consecutive "incomplete, still no header" result.
  // When that fires, the decoder and the error count are reset before
  // returning, so decode_byte() is always safe to call again immediately
  // with the next byte -- including a byte still sitting in the same
  // caller-side buffer as the one that just failed.
  ResyncResult decode_byte(uint8_t byte, uint32_t now_ms);

  // Resets the wrapped decoder, the error count and the pending
  // notification, as if newly constructed. Call this whenever a slot is
  // handed to a new connection (a reused WifiClientStream), so the "once
  // per connection" promise on consume_notify_pending() holds per
  // connection, not forever.
  void reset();

  bool has_header() const { return decoder_.has_header(); }
  const Packet& packet() const { return decoder_.packet(); }

  // True the first time this is called after decode_byte() has returned
  // header_error at least once since construction or the last reset();
  // false on every call after that, including after a later header_error
  // -- a connection that never sends a valid frame keeps tripping the
  // threshold every max_header_errors bytes, but this only ever hands back
  // true once, so a caller that checks it on every header_error result
  // still shows its diagnostic exactly once per connection.
  bool consume_notify_pending();

 private:
  FrameDecoder decoder_;
  uint16_t header_errors_ = 0;
  bool notify_pending_ = false;
  bool notified_once_ = false;
};

}  // namespace makera
