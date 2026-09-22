#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "libs/Publish.h"

namespace {

int checks;
int failures;
const char* current_test;

#define CHECK(condition)                                                      \
  do {                                                                        \
    ++checks;                                                                 \
    if (!(condition)) {                                                       \
      ++failures;                                                             \
      std::printf("  FAIL (%s:%d) %s\n", current_test, __LINE__, #condition); \
    }                                                                         \
  } while (false)

#define TEST(name)     \
  current_test = name; \
  std::printf("%s\n", name)

uint32_t read_be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

}  // namespace

int main() {
  {
    TEST("status_publish_interval_us converts a plain rate");
    CHECK(multiclient::status_publish_interval_us(5) == 200000);
    CHECK(multiclient::status_publish_interval_us(10) == 100000);
    CHECK(multiclient::status_publish_interval_us(1) == 1000000);
  }

  {
    TEST("status_publish_interval_us clamps 0 and unreasonably high rates");
    CHECK(multiclient::status_publish_interval_us(0) == multiclient::status_publish_interval_us(1));
    CHECK(multiclient::status_publish_interval_us(1000) == multiclient::status_publish_interval_us(50));
  }

  {
    TEST("publish_due is false before the interval elapses, true once it has");
    CHECK(!multiclient::publish_due(1199, 1000, 200));
    CHECK(multiclient::publish_due(1200, 1000, 200));
    CHECK(multiclient::publish_due(5000, 1000, 200));
  }

  {
    TEST("publish_due is wrap-safe at the full uint32_t range");
    // now has wrapped past 0; last_publish was just before the wrap. This
    // is the wrap publish_due() is actually correct across: the full width
    // of its uint32_t arguments. It is only correct here because callers
    // now pass raw us_ticker_read() readings (a counter that itself wraps
    // at this same 2^32 boundary) -- see the next two tests for what broke
    // when a call site divided that reading down to milliseconds first.
    const uint32_t last = 0xFFFFFFF0u;
    CHECK(!multiclient::publish_due(5, last, 200));   // only ~21 us have really passed
    CHECK(multiclient::publish_due(210, last, 200));  // ~226 us have really passed
  }

  {
    TEST("publish_due, driven by us_ticker_read()/1000, latches permanently at "
         "the derived clock's own (narrower) wrap point");
    // Pins the defect this file's API used to have, at the exact scale the
    // old call sites (WifiProvider::publish_status_if_due(),
    // SerialConsole::on_idle()) used: now_ms = us_ticker_read() / 1000.
    // us_ticker_read() wraps at 2^32 us, but the derived millisecond value
    // wraps at 4,294,967 (= 0xFFFFFFFF / 1000) -- a narrower modulus that
    // the uint32_t subtraction inside publish_due() knows nothing about.
    // last_publish_ms is rewritten only by the publish it gates, so once a
    // publish lands within one interval of that ceiling, now_ms can never
    // again climb far enough past it to satisfy the interval: the stall is
    // permanent, not periodic, confirmed below across two full wraps. This
    // test is the old call-site pattern, deliberately NOT using any renamed
    // symbol from this codebase, so it keeps demonstrating the mechanism
    // regardless of what this file's own API is named.
    const uint32_t interval_ms = 200;  // multiclient::status_publish_interval_ms(5), as it used to be named
    uint32_t raw_us = 0xFFFFFFFFu - 5'000'000u;  // ~5 s before the us_ticker wraps
    uint32_t last_publish_ms = raw_us / 1000u;
    const uint32_t step_us = 50'000u;  // 50 ms per on_idle tick

    long long publishes_before_wrap = 0;
    long long publishes_after_wrap = 0;
    bool derived_ms_wrapped = false;
    uint32_t previous_now_ms = last_publish_ms;

    // Two full us_ticker wraps' worth of ticks.
    const uint64_t total_ticks = (2ull * 0x100000000ull + 5'000'000ull) / step_us;
    for (uint64_t i = 0; i < total_ticks; ++i) {
      raw_us += step_us;
      const uint32_t now_ms = raw_us / 1000u;
      if (now_ms < previous_now_ms) derived_ms_wrapped = true;
      previous_now_ms = now_ms;

      if (multiclient::publish_due(now_ms, last_publish_ms, interval_ms)) {
        last_publish_ms = now_ms;
        if (derived_ms_wrapped) {
          ++publishes_after_wrap;
        } else {
          ++publishes_before_wrap;
        }
      }
    }

    CHECK(derived_ms_wrapped);
    CHECK(publishes_before_wrap > 0);       // worked fine before the wrap
    CHECK(publishes_after_wrap == 0);       // and never again: the permanent latch
  }

  {
    TEST("publish_due, driven by a raw undivided us_ticker_read(), keeps "
         "firing at the configured rate through two full wraps");
    // The fix: no division. Same simulation as above, but now_us is the
    // raw counter itself and the interval is in microseconds
    // (status_publish_interval_us()), so the counter's own wrap (2^32) is
    // exactly the modulus publish_due()'s signed idiom is valid across.
    const uint32_t interval_us = multiclient::status_publish_interval_us(5);
    CHECK(interval_us == 200000u);

    uint32_t raw_us = 0xFFFFFFFFu - 5'000'000u;
    uint32_t last_publish_us = raw_us;
    const uint32_t step_us = 50'000u;

    long long publishes_before_wrap = 0;
    long long publishes_after_wrap = 0;
    bool wrapped = false;
    uint32_t previous_raw_us = raw_us;

    const uint64_t total_ticks = (2ull * 0x100000000ull + 5'000'000ull) / step_us;
    for (uint64_t i = 0; i < total_ticks; ++i) {
      raw_us += step_us;
      if (raw_us < previous_raw_us) wrapped = true;
      previous_raw_us = raw_us;

      if (multiclient::publish_due(raw_us, last_publish_us, interval_us)) {
        last_publish_us = raw_us;
        if (wrapped) {
          ++publishes_after_wrap;
        } else {
          ++publishes_before_wrap;
        }
      }
    }

    CHECK(wrapped);
    CHECK(publishes_before_wrap > 0);
    // The defining property of the fix: publishing keeps happening at
    // essentially the configured rate after the wrap too, not stalling the
    // way the derived-millisecond version above does.
    const long long expected_per_wrap = static_cast<long long>(0x100000000ull / interval_us);
    CHECK(publishes_after_wrap > expected_per_wrap / 2);
  }

  {
    TEST("console_line_chunk_length caps at the per-fragment budget");
    CHECK(multiclient::console_line_chunk_length(10) == 10);
    CHECK(multiclient::console_line_chunk_length(multiclient::max_console_line_text_bytes) ==
          multiclient::max_console_line_text_bytes);
    CHECK(multiclient::console_line_chunk_length(multiclient::max_console_line_text_bytes + 1) ==
          multiclient::max_console_line_text_bytes);
  }

  {
    TEST("console_line_has_more is true only when bytes remain past this chunk");
    CHECK(!multiclient::console_line_has_more(10, 10));
    CHECK(multiclient::console_line_has_more(11, 10));
  }

  {
    TEST("build_console_line_frame writes source id, name and the more flag");
    uint8_t out[64];
    const std::size_t len =
        multiclient::build_console_line_frame(0x0102030405060708ULL, "Alice", 5, "ok\r\n", 4, true, out, sizeof(out));
    CHECK(len == 8 + 1 + 5 + 1 + 4);
    uint64_t id = 0;
    for (int i = 0; i < 8; ++i) id = (id << 8) | out[i];
    CHECK(id == 0x0102030405060708ULL);
    CHECK(out[8] == 5);
    CHECK(std::memcmp(out + 9, "Alice", 5) == 0);
    CHECK(out[14] == 1);  // more
    CHECK(std::memcmp(out + 15, "ok\r\n", 4) == 0);
  }

  {
    TEST("build_console_line_frame's more flag is 0 for the last fragment");
    uint8_t out[64];
    const std::size_t len = multiclient::build_console_line_frame(1, "A", 1, "x", 1, false, out, sizeof(out));
    CHECK(len == 8 + 1 + 1 + 1 + 1);
    CHECK(out[8 + 1 + 1] == 0);  // more, right after name
  }

  {
    TEST("build_console_line_frame supports an empty name and empty text");
    uint8_t out[16];
    const std::size_t len = multiclient::build_console_line_frame(0, nullptr, 0, "", 0, false, out, sizeof(out));
    CHECK(len == 8 + 1 + 0 + 1 + 0);
  }

  {
    TEST("build_console_line_frame refuses to write past out_capacity");
    uint8_t out[10];  // too small for an 8+1+1+1 header plus any text
    const std::size_t len = multiclient::build_console_line_frame(1, "A", 1, "hello", 5, false, out, sizeof(out));
    CHECK(len == 0);
  }

  {
    TEST("build_relay_frame prepends the source id, payload untouched");
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
    uint8_t out[8 + sizeof(payload)];
    const std::size_t len =
        multiclient::build_relay_frame(0x0102030405060708ull, payload, sizeof(payload), out, sizeof(out));
    CHECK(len == sizeof(out));
    CHECK(read_be32(out) == 0x01020304u);
    CHECK(read_be32(out + 4) == 0x05060708u);
    CHECK(std::memcmp(out + 8, payload, sizeof(payload)) == 0);
  }

  {
    TEST("build_relay_frame passes an empty payload through");
    uint8_t out[8];
    const std::size_t len = multiclient::build_relay_frame(42, nullptr, 0, out, sizeof(out));
    CHECK(len == 8);
  }

  {
    TEST("build_relay_frame refuses a payload past max_relay_payload_bytes, "
         "even with room to spare in out_capacity");
    CHECK(multiclient::max_relay_payload_bytes == 527);
    std::vector<uint8_t> payload(multiclient::max_relay_payload_bytes + 1, 0x7A);
    std::vector<uint8_t> out(8 + payload.size());
    const std::size_t len = multiclient::build_relay_frame(1, payload.data(), payload.size(), out.data(), out.size());
    CHECK(len == 0);
  }

  {
    TEST("build_relay_frame accepts exactly max_relay_payload_bytes");
    std::vector<uint8_t> payload(multiclient::max_relay_payload_bytes, 0x7A);
    std::vector<uint8_t> out(8 + payload.size());
    const std::size_t len = multiclient::build_relay_frame(1, payload.data(), payload.size(), out.data(), out.size());
    CHECK(len == out.size());
  }

  {
    TEST("build_relay_frame refuses to write past out_capacity");
    const uint8_t payload[4] = {1, 2, 3, 4};
    uint8_t out[8 + 3];  // 8-byte source id alone already exceeds this
    const std::size_t len = multiclient::build_relay_frame(1, payload, sizeof(payload), out, sizeof(out));
    CHECK(len == 0);
  }

  {
    TEST("clamp_event_path_length passes short paths through and caps long ones");
    CHECK(multiclient::clamp_event_path_length(10) == 10);
    CHECK(multiclient::clamp_event_path_length(multiclient::max_event_path_length) == multiclient::max_event_path_length);
    CHECK(multiclient::clamp_event_path_length(multiclient::max_event_path_length + 50) ==
          multiclient::max_event_path_length);
  }

  {
    TEST("build_upload_finished_event writes kind, path, size and checksum_type none");
    uint8_t out[300];
    const std::size_t len =
        multiclient::build_upload_finished_event("/sd/gcodes/part.nc", 18, 123456, out, sizeof(out));
    CHECK(len == 1 + 1 + 18 + 4 + 1);
    CHECK(out[0] == multiclient::event_kind_upload_finished);
    CHECK(out[1] == 18);
    CHECK(std::memcmp(out + 2, "/sd/gcodes/part.nc", 18) == 0);
    CHECK(read_be32(out + 2 + 18) == 123456u);
    CHECK(out[2 + 18 + 4] == multiclient::event_checksum_none);
  }

  {
    TEST("build_play_started_event writes kind and path only");
    uint8_t out[300];
    const std::size_t len = multiclient::build_play_started_event("/sd/gcodes/part.nc", 18, out, sizeof(out));
    CHECK(len == 1 + 1 + 18);
    CHECK(out[0] == multiclient::event_kind_play_started);
    CHECK(out[1] == 18);
    CHECK(std::memcmp(out + 2, "/sd/gcodes/part.nc", 18) == 0);
  }

  {
    TEST("build_job_ended_event writes kind, path and the final progress snapshot");
    uint8_t out[300];
    const std::size_t len = multiclient::build_job_ended_event("/sd/gcodes/part.nc", 18, 100, 4200, 315, out, sizeof(out));
    CHECK(len == 1 + 1 + 18 + 1 + 4 + 4);
    CHECK(out[0] == multiclient::event_kind_job_ended);
    CHECK(out[1] == 18);
    CHECK(std::memcmp(out + 2, "/sd/gcodes/part.nc", 18) == 0);
    CHECK(out[2 + 18] == 100);
    CHECK(read_be32(out + 2 + 18 + 1) == 4200u);
    CHECK(read_be32(out + 2 + 18 + 1 + 4) == 315u);
  }

  {
    TEST("build_alarm_halt_event writes kind and the halt reason");
    uint8_t out[4];
    const std::size_t len = multiclient::build_alarm_halt_event(21 /* HARD_LIMIT */, out, sizeof(out));
    CHECK(len == 2);
    CHECK(out[0] == multiclient::event_kind_alarm_halt);
    CHECK(out[1] == 21);
  }

  {
    TEST("build_control_changed_event writes kind, id and name for a real holder");
    uint8_t out[1 + 8 + 1 + 9];
    const std::size_t len = multiclient::build_control_changed_event(0x0102030405060708ull, "Office PC", 9, out, sizeof(out));
    CHECK(len == sizeof(out));
    CHECK(out[0] == multiclient::event_kind_control_changed);
    uint64_t id = 0;
    for (int i = 0; i < 8; ++i) id = (id << 8) | out[1 + i];
    CHECK(id == 0x0102030405060708ull);
    CHECK(out[9] == 9);
    CHECK(std::memcmp(out + 10, "Office PC", 9) == 0);
  }

  {
    TEST("build_control_changed_event writes id 0 / name_len 0 for \"nobody\"");
    uint8_t out[10];
    const std::size_t len = multiclient::build_control_changed_event(0, nullptr, 0, out, sizeof(out));
    CHECK(len == 10);
    CHECK(out[0] == multiclient::event_kind_control_changed);
    for (int i = 1; i <= 8; ++i) CHECK(out[i] == 0);
    CHECK(out[9] == 0);
  }

  {
    TEST("build_client_joined_event writes kind, id and name");
    uint8_t out[1 + 8 + 1 + 9];
    const std::size_t len =
        multiclient::build_client_joined_event(0x0102030405060708ull, "Office PC", 9, out, sizeof(out));
    CHECK(len == sizeof(out));
    CHECK(out[0] == multiclient::event_kind_client_joined);
    uint64_t id = 0;
    for (int i = 0; i < 8; ++i) id = (id << 8) | out[1 + i];
    CHECK(id == 0x0102030405060708ull);
    CHECK(out[9] == 9);
    CHECK(std::memcmp(out + 10, "Office PC", 9) == 0);
  }

  {
    TEST("build_client_left_event writes kind, id and name");
    uint8_t out[1 + 8 + 1 + 9];
    const std::size_t len =
        multiclient::build_client_left_event(0x0102030405060708ull, "Office PC", 9, out, sizeof(out));
    CHECK(len == sizeof(out));
    CHECK(out[0] == multiclient::event_kind_client_left);
    uint64_t id = 0;
    for (int i = 0; i < 8; ++i) id = (id << 8) | out[1 + i];
    CHECK(id == 0x0102030405060708ull);
    CHECK(out[9] == 9);
    CHECK(std::memcmp(out + 10, "Office PC", 9) == 0);
  }

  {
    TEST("build_client_joined_event writes a name at max_name_length");
    const std::string name(multiclient::max_name_length, 'x');
    uint8_t out[1 + 8 + 1 + multiclient::max_name_length];
    const std::size_t len = multiclient::build_client_joined_event(
        42, name.data(), static_cast<uint8_t>(name.size()), out, sizeof(out));
    CHECK(len == sizeof(out));
    CHECK(out[9] == multiclient::max_name_length);
    CHECK(std::memcmp(out + 10, name.data(), multiclient::max_name_length) == 0);
  }

  {
    TEST("build_client_left_event writes a name at max_name_length");
    const std::string name(multiclient::max_name_length, 'x');
    uint8_t out[1 + 8 + 1 + multiclient::max_name_length];
    const std::size_t len = multiclient::build_client_left_event(
        42, name.data(), static_cast<uint8_t>(name.size()), out, sizeof(out));
    CHECK(len == sizeof(out));
    CHECK(out[9] == multiclient::max_name_length);
    CHECK(std::memcmp(out + 10, name.data(), multiclient::max_name_length) == 0);
  }

  {
    TEST("every event builder refuses to write past out_capacity");
    uint8_t out[3];
    CHECK(multiclient::build_upload_finished_event("abc", 3, 1, out, sizeof(out)) == 0);
    CHECK(multiclient::build_play_started_event("abc", 3, out, sizeof(out)) == 0);
    CHECK(multiclient::build_job_ended_event("abc", 3, 0, 0, 0, out, sizeof(out)) == 0);
    uint8_t tiny[1];
    CHECK(multiclient::build_alarm_halt_event(1, tiny, sizeof(tiny)) == 0);
    CHECK(multiclient::build_control_changed_event(1, "Office PC", 9, tiny, sizeof(tiny)) == 0);
    CHECK(multiclient::build_client_joined_event(1, "Office PC", 9, tiny, sizeof(tiny)) == 0);
    CHECK(multiclient::build_client_left_event(1, "Office PC", 9, tiny, sizeof(tiny)) == 0);
  }

  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
