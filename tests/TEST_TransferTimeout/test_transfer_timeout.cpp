#include <cstdint>
#include <cstdio>

#include "libs/TransferTimeout.h"

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

constexpr uint32_t second_us = 1000000;

using transfer_timeout::download_stalled;
using transfer_timeout::download_stall_us;

}  // namespace

int main() {
  {
    TEST("the download stall limit is 10 seconds");
    CHECK(download_stall_us == 10 * second_us);
  }

  {
    TEST("a controller still retrying is not stalled: its retry comes every 5 s");
    CHECK(!download_stalled(0, 0));
    CHECK(!download_stalled(0, 5 * second_us));
    CHECK(!download_stalled(0, 5 * second_us + 500000));
    CHECK(!download_stalled(0, 9 * second_us));
  }

  {
    TEST("stalled only once more than 10 s have passed with no request");
    CHECK(!download_stalled(0, 10 * second_us));
    CHECK(download_stalled(0, 10 * second_us + 1));
    CHECK(download_stalled(0, 11 * second_us));
  }

  {
    TEST("stalled well before the old 29 s limit");
    CHECK(download_stalled(0, 15 * second_us));
    CHECK(download_stalled(0, 29 * second_us));
  }

  {
    TEST("measured from the last request, not from the start of the download");
    const uint32_t last_request = 100 * second_us;
    CHECK(!download_stalled(last_request, last_request + 9 * second_us));
    CHECK(download_stalled(last_request, last_request + 11 * second_us));
  }

  {
    TEST("correct across a wrap of the microsecond counter");
    const uint32_t before_wrap = UINT32_MAX - 2 * second_us;
    CHECK(!download_stalled(before_wrap, before_wrap + 9 * second_us));
    CHECK(download_stalled(before_wrap, before_wrap + 11 * second_us));
  }

  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
