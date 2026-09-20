#include "Publish.h"

#include <algorithm>
#include <cstring>

namespace multiclient {

namespace {
constexpr uint16_t min_status_publish_hz = 1;
constexpr uint16_t max_status_publish_hz = 50;
}  // namespace

uint32_t status_publish_interval_ms(uint16_t hz) {
  const uint16_t clamped = std::clamp(hz, min_status_publish_hz, max_status_publish_hz);
  return 1000u / clamped;
}

bool publish_due(uint32_t now_ms, uint32_t last_publish_ms, uint32_t interval_ms) {
  return static_cast<int32_t>(now_ms - last_publish_ms) >= static_cast<int32_t>(interval_ms);
}

std::size_t build_console_line_frame(uint64_t source_id, const char* source_name, uint8_t source_name_len,
                                      const char* text_chunk, std::size_t chunk_length, bool more, uint8_t* out,
                                      std::size_t out_capacity) {
  const std::size_t length = 8 + 1 + source_name_len + 1 + chunk_length;
  if (length > out_capacity) return 0;

  std::size_t offset = 0;
  for (int i = 0; i < 8; ++i) out[offset + i] = static_cast<uint8_t>(source_id >> (8 * (7 - i)));
  offset += 8;
  out[offset++] = source_name_len;
  if (source_name_len != 0) {
    std::memcpy(out + offset, source_name, source_name_len);
    offset += source_name_len;
  }
  out[offset++] = more ? 1 : 0;
  if (chunk_length != 0) {
    std::memcpy(out + offset, text_chunk, chunk_length);
    offset += chunk_length;
  }
  return offset;
}

uint8_t clamp_event_path_length(std::size_t length) {
  return length > max_event_path_length ? static_cast<uint8_t>(max_event_path_length) : static_cast<uint8_t>(length);
}

namespace {
// Appends kind(1) + path_len(1) + path to `out` at offset 0. Returns the
// offset just past the path, or 0 if it did not fit -- every event payload
// in this file starts this way.
std::size_t append_kind_and_path(uint8_t kind, const char* path, uint8_t path_len, uint8_t* out,
                                  std::size_t out_capacity) {
  const std::size_t needed = 1 + 1 + path_len;
  if (needed > out_capacity) return 0;
  out[0] = kind;
  out[1] = path_len;
  if (path_len != 0) std::memcpy(out + 2, path, path_len);
  return 2 + path_len;
}
}  // namespace

std::size_t build_upload_finished_event(const char* path, uint8_t path_len, uint32_t size, uint8_t* out,
                                         std::size_t out_capacity) {
  std::size_t offset = append_kind_and_path(event_kind_upload_finished, path, path_len, out, out_capacity);
  if (offset == 0) return 0;
  if (offset + 4 + 1 > out_capacity) return 0;
  for (int i = 0; i < 4; ++i) out[offset + i] = static_cast<uint8_t>(size >> (8 * (3 - i)));
  offset += 4;
  out[offset++] = event_checksum_none;
  return offset;
}

std::size_t build_play_started_event(const char* path, uint8_t path_len, uint8_t* out, std::size_t out_capacity) {
  return append_kind_and_path(event_kind_play_started, path, path_len, out, out_capacity);
}

std::size_t build_job_ended_event(const char* path, uint8_t path_len, uint8_t percent_complete,
                                   uint32_t played_lines, uint32_t elapsed_secs, uint8_t* out,
                                   std::size_t out_capacity) {
  std::size_t offset = append_kind_and_path(event_kind_job_ended, path, path_len, out, out_capacity);
  if (offset == 0) return 0;
  if (offset + 1 + 4 + 4 > out_capacity) return 0;
  out[offset++] = percent_complete;
  for (int i = 0; i < 4; ++i) out[offset + i] = static_cast<uint8_t>(played_lines >> (8 * (3 - i)));
  offset += 4;
  for (int i = 0; i < 4; ++i) out[offset + i] = static_cast<uint8_t>(elapsed_secs >> (8 * (3 - i)));
  offset += 4;
  return offset;
}

std::size_t build_alarm_halt_event(uint8_t halt_reason, uint8_t* out, std::size_t out_capacity) {
  if (out_capacity < 2) return 0;
  out[0] = event_kind_alarm_halt;
  out[1] = halt_reason;
  return 2;
}

}  // namespace multiclient
