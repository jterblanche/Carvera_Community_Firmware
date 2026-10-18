#include <cstdint>
#include <cstdio>

#include "libs/ClientTable.h"
#include "libs/Hello.h"
#include "libs/UsbActivity.h"

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

constexpr uint32_t second = 1000000;

multiclient::Address address(uint8_t last) {
  multiclient::Address addr;
  addr.ip[0] = 192;
  addr.ip[1] = 168;
  addr.ip[2] = 1;
  addr.ip[3] = last;
  addr.port = 5000;
  return addr;
}

// What SerialConsole::on_idle() does with the USB session: when it has
// expired, a client-left event is published if the link had said hello, and
// its identity is cleared. Returns true if a client-left would be published.
bool idle_check(multiclient::ClientTable& table, const multiclient::UsbActivity& activity, uint32_t now_us) {
  const multiclient::Client* usb = table.usb();
  if (usb == nullptr || !activity.session_expired(usb->hello_window_started, now_us)) return false;
  const bool left = usb->identified;
  table.clear_usb_identity();
  return left;
}

// A USB link that has said hello, last heard from at `t0`.
void identified_usb(multiclient::ClientTable& table, multiclient::UsbActivity& activity, uint32_t t0) {
  table.set_usb_present(true, t0);
  table.start_usb_hello_window(t0);
  multiclient::Client* usb = table.usb();
  multiclient::set_identity(*usb, 0x1111, "pc", 2);
  activity.last_activity_us = t0;
}

}  // namespace

int main() {
  {
    TEST("a quiet USB link with receive handling on still ends its session after the idle timeout");
    multiclient::ClientTable table;
    multiclient::UsbActivity activity;
    identified_usb(table, activity, 1000);
    CHECK(!idle_check(table, activity, 1000 + multiclient::usb_idle_timeout_us - 1));
    CHECK(table.usb()->identified);
    CHECK(idle_check(table, activity, 1000 + multiclient::usb_idle_timeout_us));
    CHECK(!table.usb()->identified);
  }

  {
    TEST("an upload longer than the idle timeout keeps the uploader's session, with no client-left");
    multiclient::ClientTable table;
    multiclient::UsbActivity activity;
    identified_usb(table, activity, 1000);
    activity.set_rx_enabled(false, 1000);
    // The idle work run during the decompress, every few blocks.
    bool left = false;
    for (uint32_t t = 1000; t <= 1000 + 60 * second; t += 250000) left = idle_check(table, activity, t) || left;
    CHECK(!left);
    CHECK(table.usb()->identified);
    CHECK(table.usb()->id == 0x1111);
  }

  {
    TEST("output still reaches the uploader while WiFi controllers have said hello");
    multiclient::ClientTable table;
    multiclient::UsbActivity activity;
    identified_usb(table, activity, 1000);
    for (uint8_t i = 1; i <= 2; ++i) {
      const int slot = table.add_wifi(address(i), 1000);
      CHECK(slot >= 0);
      multiclient::set_identity(*table.wifi_at(slot), 0x2000 + i, "wifi", 4);
    }
    activity.set_rx_enabled(false, 1000);
    idle_check(table, activity, 1000 + 30 * second);
    CHECK(!multiclient::must_identify_first(table, table.usb()));
  }

  {
    TEST("switching receive handling back on restamps the activity time");
    multiclient::UsbActivity activity;
    activity.last_activity_us = 1000;
    activity.set_rx_enabled(false, 1000);
    activity.set_rx_enabled(true, 1000 + 40 * second);
    CHECK(activity.last_activity_us == 1000 + 40 * second);
  }

  {
    TEST("switching receive handling back on clears the flag, so the checks resume from then");
    // Every way out of an upload or download, refusals and errors included,
    // switches receive handling back on through this same call.
    multiclient::ClientTable table;
    multiclient::UsbActivity activity;
    identified_usb(table, activity, 1000);
    activity.set_rx_enabled(false, 1000);
    CHECK(activity.rx_detached);
    const uint32_t end = 1000 + 25 * second;
    activity.set_rx_enabled(true, end);
    CHECK(!activity.rx_detached);
    CHECK(!idle_check(table, activity, end));
    CHECK(!idle_check(table, activity, end + multiclient::usb_idle_timeout_us - 1));
    CHECK(table.usb()->identified);
    CHECK(idle_check(table, activity, end + multiclient::usb_idle_timeout_us));
    CHECK(!table.usb()->identified);
  }

  {
    TEST("a download longer than both timeouts does not end the session or revert the baud when it ends");
    // A download keeps the idle work off until it has switched receive
    // handling back on, so the first check comes just after that.
    multiclient::ClientTable table;
    multiclient::UsbActivity activity;
    identified_usb(table, activity, 1000);
    activity.set_rx_enabled(false, 1000);
    const uint32_t end = 1000 + 90 * second;
    activity.set_rx_enabled(true, end);
    CHECK(!idle_check(table, activity, end + 1));
    CHECK(!activity.temporary_baud_expired(end + 1));
    CHECK(table.usb()->identified);
  }

  {
    TEST("a temporary baud rate is not reverted during a transfer");
    multiclient::UsbActivity activity;
    activity.last_activity_us = 1000;
    CHECK(!activity.temporary_baud_expired(1000 + multiclient::temporary_baud_timeout_us - 1));
    CHECK(activity.temporary_baud_expired(1000 + multiclient::temporary_baud_timeout_us));
    activity.set_rx_enabled(false, 1000);
    CHECK(!activity.temporary_baud_expired(1000 + 60 * second));
    const uint32_t end = 1000 + 60 * second;
    activity.set_rx_enabled(true, end);
    CHECK(!activity.temporary_baud_expired(end + multiclient::temporary_baud_timeout_us - 1));
    CHECK(activity.temporary_baud_expired(end + multiclient::temporary_baud_timeout_us));
  }

  {
    TEST("a link that has not started its hello window never expires, transfer or not");
    multiclient::UsbActivity activity;
    activity.last_activity_us = 1000;
    CHECK(!activity.session_expired(false, 1000 + 60 * second));
    activity.set_rx_enabled(false, 1000);
    CHECK(!activity.session_expired(false, 1000 + 60 * second));
  }

  {
    TEST("both checks hold across a wrap of the microsecond counter");
    multiclient::UsbActivity activity;
    const uint32_t start = 0xFFFFFFFFu - 2 * second;
    activity.last_activity_us = start;
    activity.set_rx_enabled(false, start);
    CHECK(!activity.session_expired(true, start + 20 * second));
    CHECK(!activity.temporary_baud_expired(start + 20 * second));
    const uint32_t end = start + 20 * second;  // past the wrap
    activity.set_rx_enabled(true, end);
    CHECK(!activity.session_expired(true, end + multiclient::usb_idle_timeout_us - 1));
    CHECK(activity.session_expired(true, end + multiclient::usb_idle_timeout_us));
    CHECK(activity.temporary_baud_expired(end + multiclient::temporary_baud_timeout_us));
  }

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
