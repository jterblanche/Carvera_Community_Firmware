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
    TEST("status_publish_interval_ms converts a plain rate");
    CHECK(multiclient::status_publish_interval_ms(5) == 200);
    CHECK(multiclient::status_publish_interval_ms(10) == 100);
    CHECK(multiclient::status_publish_interval_ms(1) == 1000);
  }

  {
    TEST("status_publish_interval_ms clamps 0 and unreasonably high rates");
    CHECK(multiclient::status_publish_interval_ms(0) == multiclient::status_publish_interval_ms(1));
    CHECK(multiclient::status_publish_interval_ms(1000) == multiclient::status_publish_interval_ms(50));
  }

  {
    TEST("publish_due is false before the interval elapses, true once it has");
    CHECK(!multiclient::publish_due(1199, 1000, 200));
    CHECK(multiclient::publish_due(1200, 1000, 200));
    CHECK(multiclient::publish_due(5000, 1000, 200));
  }

  {
    TEST("publish_due is wrap-safe");
    // now_ms has wrapped past 0; last_publish_ms was just before the wrap.
    const uint32_t last = 0xFFFFFFF0u;
    CHECK(!multiclient::publish_due(5, last, 200));   // only ~21 ms have really passed
    CHECK(multiclient::publish_due(210, last, 200));  // ~226 ms have really passed
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
    TEST("every event builder refuses to write past out_capacity");
    uint8_t out[3];
    CHECK(multiclient::build_upload_finished_event("abc", 3, 1, out, sizeof(out)) == 0);
    CHECK(multiclient::build_play_started_event("abc", 3, out, sizeof(out)) == 0);
    CHECK(multiclient::build_job_ended_event("abc", 3, 0, 0, 0, out, sizeof(out)) == 0);
    uint8_t tiny[1];
    CHECK(multiclient::build_alarm_halt_event(1, tiny, sizeof(tiny)) == 0);
  }

  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
