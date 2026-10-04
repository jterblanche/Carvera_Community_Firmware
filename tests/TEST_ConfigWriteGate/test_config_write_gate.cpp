#include <cstdio>

#include "libs/ConfigWriteGate.h"

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

}  // namespace

int main() {
  {
    TEST("refuses_write: idle, alarm and sleeping accept a write");
    CHECK(!config_write_gate::refuses_write({/*idle=*/true, /*alarm=*/false, /*sleeping=*/false}));
    CHECK(!config_write_gate::refuses_write({/*idle=*/false, /*alarm=*/true, /*sleeping=*/false}));
    CHECK(!config_write_gate::refuses_write({/*idle=*/false, /*alarm=*/false, /*sleeping=*/true}));
  }

  {
    TEST("refuses_write: everything else -- run, hold, home, suspend, wait, tool -- refuses a write");
    CHECK(config_write_gate::refuses_write({/*idle=*/false, /*alarm=*/false, /*sleeping=*/false}));
  }

  {
    TEST("refuses_write: a default MachineState (no flag set) refuses a write");
    CHECK(config_write_gate::refuses_write(config_write_gate::MachineState{}));
  }

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
