#pragma once

#include <cstddef>
#include <cstdint>

#include "ClientTable.h"

// Wire encoding and pure decisions for the messages the machine publishes on
// its own initiative, without a request: proactive status, the published
// console line, and events. Plain C++, no Kernel or mbed dependency, so it
// builds and runs on the host (see tests/TEST_Publish). The transports
// (WifiProvider, SerialConsole) own the actual sending; this file only
// decides *whether* and *what*.
namespace multiclient {

// --- Status publish timing ---

// `multi_client.status_publish_hz` falls back to this when unset.
constexpr uint16_t default_status_publish_hz = 5;

// Converts a configured rate into an interval in microseconds. A
// configured value of 0, or anything above 50 Hz, is clamped -- 0 would
// otherwise mean "publish every tick", and an unreasonably high rate would
// spend more time sending status than doing anything else. 50 Hz is far
// above the 5 Hz default and above any plausible reason to raise it.
// Microseconds, not milliseconds: see publish_due() below for why.
uint32_t status_publish_interval_us(uint16_t hz);

// True once `interval` has passed since `last_publish`. Wrap-safe via the
// signed-subtraction idiom every millisecond-scale timeout in this codebase
// used to rely on -- but that idiom is only correct when the value wraps at
// the full width of its type. Callers MUST pass raw us_ticker_read()
// readings (or a difference of two such readings), never a value derived by
// dividing one down to a coarser unit: a narrowed value wraps at a smaller
// number than 2^32, and after that wrap this idiom sees a large negative
// difference instead of a small one, so `now` never overtakes `last` again
// -- `last_publish` latches at its pre-wrap value forever, because it is
// only ever rewritten by the very publish this check gates. `interval` is
// in the same units as `now`/`last_publish` -- this codebase always calls
// it in microseconds now, via status_publish_interval_us() above.
bool publish_due(uint32_t now, uint32_t last_publish, uint32_t interval);

// --- Published console line (0x69) fragmentation ---

// Conservative per-fragment text budget: the 535 B new-message cap (see the
// protocol contract, section 1) minus the worst-case header -- 8 B source
// id + 1 B name_len + max_name_length B name + 1 B more -- so a fragment's
// size never depends on how long the *particular* sender's name happens to
// be. A frame built with a shorter name simply has a little room to spare.
constexpr std::size_t max_console_line_text_bytes = 535 - (8 + 1 + max_name_length + 1);

// How many bytes of a line, `remaining` bytes long, the next fragment
// should carry.
constexpr std::size_t console_line_chunk_length(std::size_t remaining) {
  return remaining > max_console_line_text_bytes ? max_console_line_text_bytes : remaining;
}

// True if another fragment follows one of `chunk_length` bytes, out of
// `remaining` bytes left before it was taken.
constexpr bool console_line_has_more(std::size_t remaining, std::size_t chunk_length) {
  return remaining > chunk_length;
}

// Builds one published-console-line payload: source_id(8) + source_name_len(1)
// + source_name + more(1) + text_chunk. `text_chunk`/`chunk_length` is
// already the slice this one fragment carries -- the caller applies
// console_line_chunk_length()/console_line_has_more() in a loop to walk a
// longer line. Returns the payload length, or 0 if it would not fit in
// out_capacity (callers size `out` from max_console_line_text_bytes plus
// the fixed header, so this should not happen in practice).
std::size_t build_console_line_frame(uint64_t source_id, const char* source_name, uint8_t source_name_len,
                                      const char* text_chunk, std::size_t chunk_length, bool more, uint8_t* out,
                                      std::size_t out_capacity);

// --- Events (0x68) ---

// kind values, see the protocol contract's message catalogue (0x68 event).
constexpr uint8_t event_kind_upload_finished = 1;
constexpr uint8_t event_kind_play_started = 2;
constexpr uint8_t event_kind_job_ended = 3;
constexpr uint8_t event_kind_alarm_halt = 4;
constexpr uint8_t event_kind_control_changed = 5;

// event_kind_upload_finished's checksum_type. This build always writes
// event_checksum_none -- see the change explanation for why no digest is
// attached.
constexpr uint8_t event_checksum_none = 0;
constexpr uint8_t event_checksum_md5 = 1;

// The u8 length prefix's own ceiling for a path in an event payload.
constexpr std::size_t max_event_path_length = 250;

// Clamps `length` to max_event_path_length, for a path the caller does not
// otherwise bound (a filename can in principle be longer than the wire
// format's one-byte length prefix allows).
uint8_t clamp_event_path_length(std::size_t length);

// "upload finished": kind(1) + path_len(1) + path + size(4, BE) +
// checksum_type(1) + checksum(0 B, since checksum_type is always none
// here). Returns the payload length, or 0 if it would not fit.
std::size_t build_upload_finished_event(const char* path, uint8_t path_len, uint32_t size, uint8_t* out,
                                         std::size_t out_capacity);

// "play started": kind(1) + path_len(1) + path.
std::size_t build_play_started_event(const char* path, uint8_t path_len, uint8_t* out, std::size_t out_capacity);

// "job ended": kind(1) + path_len(1) + path + percent_complete(1) +
// played_lines(4, BE) + elapsed_secs(4, BE) -- the same final-progress
// snapshot the status query already freezes when playback stops
// (Player::save_last_progress), whatever the reason it stopped. This
// firmware does not attempt to classify success vs. abort vs. halt here;
// the snapshot already tells a client everything it would otherwise have
// polled for.
std::size_t build_job_ended_event(const char* path, uint8_t path_len, uint8_t percent_complete,
                                   uint32_t played_lines, uint32_t elapsed_secs, uint8_t* out,
                                   std::size_t out_capacity);

// "alarm/halt": kind(1) + reason(1), the existing HALT_REASON value
// (Kernel.h) -- reusing it rather than inventing a second vocabulary for
// the same fact.
std::size_t build_alarm_halt_event(uint8_t halt_reason, uint8_t* out, std::size_t out_capacity);

// "control changed": kind(1) + holder_id(8, BE) + holder_name_len(1) +
// holder_name. `holder_id == 0 && holder_name_len == 0` together mean
// nobody has control (the holder disconnected or silently dropped) -- a
// real client's id is vanishingly unlikely to be exactly 0, since it is a
// random 64-bit value chosen once and kept forever (see the identify
// handshake, ClientTable.h). Published by the control gate (libs/
// ControlToken.h) whenever the holder actually changes, including to
// nobody.
std::size_t build_control_changed_event(uint64_t holder_id, const char* holder_name, uint8_t holder_name_len,
                                         uint8_t* out, std::size_t out_capacity);

}  // namespace multiclient
