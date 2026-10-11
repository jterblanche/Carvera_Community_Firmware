#include <cstdint>
#include <cstdio>

#include "libs/DownloadRetry.h"

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

using download_retry::repeats_data_packet;

// The request handling of Player::download_command, reduced to the frame
// sent and the data packet number, so a sequence of requests can be played
// through it. A data request's packet number is read from the request
// unless the retry rule says to repeat the last packet.
struct Download {
  uint8_t last_sent = PTYPE_FILE_MD5;  // the MD5 frame is sent unasked
  bool retry = false;
  long packet = 0;

  // Returns the frame sent in answer to `request`.
  uint8_t answer(uint8_t request, long requested_packet = 0) {
    uint8_t cmd = request;
    if (cmd == PTYPE_FILE_RETRY) {
      cmd = last_sent;
      retry = repeats_data_packet(last_sent);
    }
    if (cmd == PTYPE_FILE_DATA) {
      if (!retry) {
        packet = requested_packet;
      }
      retry = false;
    }
    last_sent = cmd;
    return cmd;
  }
};

}  // namespace

int main() {
  {
    TEST("a retry after a data packet repeats it");
    CHECK(repeats_data_packet(PTYPE_FILE_DATA));
  }

  {
    TEST("a retry after the MD5 or view frame does not repeat a data packet");
    CHECK(!repeats_data_packet(PTYPE_FILE_MD5));
    CHECK(!repeats_data_packet(PTYPE_FILE_VIEW));
  }

  {
    TEST("the frame type is compared as a byte, whatever the sign of char");
    const char data_as_char = static_cast<char>(PTYPE_FILE_DATA);
    CHECK(repeats_data_packet(data_as_char));
  }

  {
    TEST("MD5, retry, view, data request 1 sends packet 1");
    Download d;
    CHECK(d.answer(PTYPE_FILE_RETRY) == PTYPE_FILE_MD5);
    CHECK(d.answer(PTYPE_FILE_VIEW) == PTYPE_FILE_VIEW);
    CHECK(d.answer(PTYPE_FILE_DATA, 1) == PTYPE_FILE_DATA);
    CHECK(d.packet == 1);
    CHECK(d.answer(PTYPE_FILE_DATA, 2) == PTYPE_FILE_DATA);
    CHECK(d.packet == 2);
  }

  {
    TEST("MD5, retry, data request 1 sends packet 1");
    Download d;
    CHECK(d.answer(PTYPE_FILE_RETRY) == PTYPE_FILE_MD5);
    CHECK(d.answer(PTYPE_FILE_DATA, 1) == PTYPE_FILE_DATA);
    CHECK(d.packet == 1);
  }

  {
    TEST("view, retry, data request 1 sends packet 1");
    Download d;
    CHECK(d.answer(PTYPE_FILE_VIEW) == PTYPE_FILE_VIEW);
    CHECK(d.answer(PTYPE_FILE_RETRY) == PTYPE_FILE_VIEW);
    CHECK(d.answer(PTYPE_FILE_DATA, 1) == PTYPE_FILE_DATA);
    CHECK(d.packet == 1);
  }

  {
    TEST("a retry during the data packets repeats the last packet once");
    Download d;
    d.answer(PTYPE_FILE_VIEW);
    d.answer(PTYPE_FILE_DATA, 1);
    d.answer(PTYPE_FILE_DATA, 2);
    CHECK(d.answer(PTYPE_FILE_RETRY) == PTYPE_FILE_DATA);
    CHECK(d.packet == 2);
    CHECK(d.answer(PTYPE_FILE_DATA, 3) == PTYPE_FILE_DATA);
    CHECK(d.packet == 3);
  }

  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
