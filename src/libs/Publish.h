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

// Conservative per-fragment text budget: the 535 B new-message cap minus
// the worst-case header -- 8 B source id + 1 B name_len + max_name_length
// B name + 1 B more -- so a fragment's size never depends on how long the
// *particular* sender's name happens to be. A frame built with a shorter
// name simply has a little room to spare.
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

// --- Relay (0x67) ---

// Cap on a relay message's own payload, leaving room for the 8-byte source
// id this file prepends before the machine re-sends it, so the relayed-out
// frame still fits the 535 B new-message cap: 527 + 8 = 535. A sender that
// ignores this and sends more is not truncated -- build_relay_frame()
// below just refuses to build anything for it.
constexpr std::size_t max_relay_payload_bytes = 535 - 8;

// Builds source_id(8, BE) + payload verbatim, for the machine to send to
// every *other* identified client. The payload itself is never inspected --
// copied byte for byte. Returns 0, meaning "do not send this", if
// payload_length exceeds max_relay_payload_bytes or the result would not
// fit in out_capacity.
std::size_t build_relay_frame(uint64_t source_id, const uint8_t* payload, std::size_t payload_length, uint8_t* out,
                               std::size_t out_capacity);

// --- Events (0x68) ---

// kind values for the 0x68 event.
constexpr uint8_t event_kind_upload_finished = 1;
constexpr uint8_t event_kind_play_started = 2;
constexpr uint8_t event_kind_job_ended = 3;
constexpr uint8_t event_kind_alarm_halt = 4;
constexpr uint8_t event_kind_control_changed = 5;
constexpr uint8_t event_kind_client_joined = 6;
constexpr uint8_t event_kind_client_left = 7;
constexpr uint8_t event_kind_job_start = 8;

// The checksum_type of event_kind_upload_finished and
// event_kind_play_started: event_checksum_none when the machine found no
// usable digest to publish, event_checksum_md5 when it did (16 raw bytes
// follow the checksum_type byte).
constexpr uint8_t event_checksum_none = 0;
constexpr uint8_t event_checksum_md5 = 1;

// Raw byte length of an MD5 digest, as published after event_checksum_md5.
constexpr std::size_t md5_digest_bytes = 16;

// Decodes 32 hex characters into the 16 raw bytes they represent, two
// characters per byte, most significant nibble first. The caller must
// already know `hex` holds 32 valid hex characters (upper or lower case) --
// this does not re-validate them.
void decode_md5_hex(const char* hex, uint8_t* out16);

// The u8 length prefix's own ceiling for a path in an event payload.
constexpr std::size_t max_event_path_length = 250;

// Clamps `length` to max_event_path_length, for a path the caller does not
// otherwise bound (a filename can in principle be longer than the wire
// format's one-byte length prefix allows).
uint8_t clamp_event_path_length(std::size_t length);

// The length to publish for `path` (`length` bytes) in an event: without any
// line ending left at its end, then clamped as above. A path read from a
// command line can keep that line's newline (shift_parameter() only splits
// on a space), but the newline is not part of the file's name.
uint8_t event_path_length(const char* path, std::size_t length);

// "upload finished": kind(1) + path_len(1) + path + size(4, BE) +
// checksum_type(1) + checksum(0 or md5_digest_bytes B). `md5_digest` is a
// pointer to 16 raw bytes already decoded from the upload's own .md5
// sidecar (decode_md5_hex() above) when the upload path found a usable one,
// or nullptr when it did not -- this never hashes the file itself. Returns
// the payload length, or 0 if it would not fit.
std::size_t build_upload_finished_event(const char* path, uint8_t path_len, uint32_t size,
                                         const uint8_t* md5_digest, uint8_t* out, std::size_t out_capacity);

// "play started": kind(1) + path_len(1) + path + size(4, BE) +
// checksum_type(1) + checksum(0 or md5_digest_bytes B) -- the same fields,
// in the same order, as build_upload_finished_event() above, so a
// controller can compare the playing file with a copy it already holds.
// `size` is the file's size in bytes. `md5_digest` is 16 raw bytes decoded
// from the .md5 sidecar file stored beside the played file, or nullptr when
// there is no usable one; the file itself is never hashed for this. The
// sidecar is written by an upload, so a file put on the card any other way
// has none, and one changed without its sidecar being rewritten can carry a
// digest that no longer matches it: compare the size as well. Returns the
// payload length, or 0 if it would not fit.
std::size_t build_play_started_event(const char* path, uint8_t path_len, uint32_t size, const uint8_t* md5_digest,
                                      uint8_t* out, std::size_t out_capacity);

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

// "client joined": kind(1) + client_id(8, BE) + name_len(1) + name -- same
// layout as build_control_changed_event() above, so a controller decodes it
// with the code it already has. Published once, in the hello path, for a
// client whose identity was just set (WifiProvider and SerialConsole both
// call it there and nowhere else): never for a client that was already
// identified re-sending hello, and never for a reconnect under an id
// already in the table -- the hello path drops that stale entry first, so
// from a peer's point of view the same controller is still there, on a new
// socket, and publishing "joined" for it would be noise.
std::size_t build_client_joined_event(uint64_t client_id, const char* name, uint8_t name_len, uint8_t* out,
                                       std::size_t out_capacity);

// "client left": same layout as build_client_joined_event() above. Published
// when an identified client is removed from the table or loses its
// identity for real -- the WiFi reaping pass and the USB session-expiry
// check -- but not for the stale-duplicate removal in either hello path
// (the same controller reconnecting on a new socket, not someone leaving)
// and not for the wholesale clear_wifi()/clear_usb_identity() a protocol
// switch does, which has no audience left to publish to by the time it
// runs.
std::size_t build_client_left_event(uint64_t client_id, const char* name, uint8_t name_len, uint8_t* out,
                                     std::size_t out_capacity);

// "job start" phases: a held start first hashes the file (hashing, sent
// when the hold begins and once a second after, with no checksum), then
// waits while other controllers load it (waiting, sent when hashing ends
// and once a second after, with the computed checksum), then it ends one
// of two ways, each sent once (starting, just before the job's own
// play-started; cancelled, and the job does not run). Starting and
// cancelled can follow hashing directly.
constexpr uint8_t job_start_phase_waiting = 0;
constexpr uint8_t job_start_phase_starting = 1;
constexpr uint8_t job_start_phase_cancelled = 2;
constexpr uint8_t job_start_phase_hashing = 3;

// "job start" reasons: why the hold ended, or job_start_reason_waiting
// while it has not.
constexpr uint8_t job_start_reason_waiting = 0;
constexpr uint8_t job_start_reason_all_ready = 1;     // starting: every awaited controller is ready
constexpr uint8_t job_start_reason_time_limit = 2;    // starting: multi_client.start_wait_s ran out
constexpr uint8_t job_start_reason_start_now = 3;     // starting: start-now
constexpr uint8_t job_start_reason_aborted = 4;       // cancelled: abort
constexpr uint8_t job_start_reason_starter_left = 5;  // cancelled: the controller that started it left
constexpr uint8_t job_start_reason_halted = 6;        // cancelled: the machine entered alarm/halt

// The most controllers a "job start" event lists as not ready: one per
// client the table can hold, every WiFi slot plus USB.
constexpr std::size_t max_job_start_not_ready = max_wifi_clients + 1;

// The fields of one "job start" event. `md5_digest` is 16 raw bytes or
// nullptr, as for play-started. `not_ready_ids` holds `not_ready_count`
// client ids; past max_job_start_not_ready they are not sent.
struct JobStartEvent {
  const char* path = nullptr;
  uint8_t path_len = 0;
  uint32_t size = 0;
  const uint8_t* md5_digest = nullptr;
  uint16_t start_id = 0;
  uint8_t phase = job_start_phase_waiting;
  uint8_t reason = job_start_reason_waiting;
  uint8_t seconds_left = 0;
  uint64_t starter_id = 0;
  const uint64_t* not_ready_ids = nullptr;
  uint8_t not_ready_count = 0;
};

// Longest "job start" payload: the file fields at their longest, the fixed
// wait fields, and a full not-ready list.
constexpr std::size_t max_job_start_event_length = 1 + 1 + max_event_path_length + 4 + 1 + md5_digest_bytes +
                                                   2 + 1 + 1 + 1 + 8 + 1 + 8 * max_job_start_not_ready;

// "job start": kind(1) + path_len(1) + path + size(4, BE) +
// checksum_type(1) + checksum(0 or md5_digest_bytes B) -- byte for byte
// the play-started layout up to here, so a controller reads the file with
// the same code -- then start_id(2, BE) + phase(1) + reason(1) +
// seconds_left(1) + starter_id(8, BE) + not_ready_count(1) +
// not_ready_ids(8 B each, BE). Published while a job start is held for
// other controllers to load the file (libs/JobStartWait.h). Returns the
// payload length, or 0 if it would not fit.
std::size_t build_job_start_event(const JobStartEvent& event, uint8_t* out, std::size_t out_capacity);

}  // namespace multiclient
