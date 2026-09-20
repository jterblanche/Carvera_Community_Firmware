#include <cstdint>
#include <cstdio>

#include "libs/ClientTable.h"

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

multiclient::Address address(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
  multiclient::Address addr;
  addr.ip[0] = a;
  addr.ip[1] = b;
  addr.ip[2] = c;
  addr.ip[3] = d;
  addr.port = port;
  return addr;
}

}  // namespace

int main() {
  {
    TEST("a fresh table has no WiFi clients and no USB entry");
    multiclient::ClientTable table;
    CHECK(table.wifi_count() == 0);
    CHECK(table.usb() == nullptr);
    CHECK(table.find_wifi(address(192, 168, 1, 10, 2222)) == -1);
  }

  {
    TEST("WiFi clients are added up to the cap, in order");
    multiclient::ClientTable table;
    const int first = table.add_wifi(address(192, 168, 1, 10, 51000), 1000);
    const int second = table.add_wifi(address(192, 168, 1, 11, 51001), 1001);
    const int third = table.add_wifi(address(192, 168, 1, 12, 51002), 1002);
    CHECK(first == 0);
    CHECK(second == 1);
    CHECK(third == 2);
    CHECK(table.wifi_count() == multiclient::max_wifi_clients);
  }

  {
    TEST("a WiFi client beyond the cap is refused, not evicted");
    multiclient::ClientTable table;
    table.add_wifi(address(10, 0, 0, 1, 1), 0);
    table.add_wifi(address(10, 0, 0, 2, 2), 0);
    table.add_wifi(address(10, 0, 0, 3, 3), 0);
    const int fourth = table.add_wifi(address(10, 0, 0, 4, 4), 0);
    CHECK(fourth == -1);
    CHECK(table.wifi_count() == multiclient::max_wifi_clients);
    // the three already admitted are untouched
    CHECK(table.find_wifi(address(10, 0, 0, 1, 1)) == 0);
    CHECK(table.find_wifi(address(10, 0, 0, 2, 2)) == 1);
    CHECK(table.find_wifi(address(10, 0, 0, 3, 3)) == 2);
  }

  {
    TEST("re-adding a connected address is idempotent, not a second slot");
    multiclient::ClientTable table;
    const int first = table.add_wifi(address(1, 2, 3, 4, 9000), 5);
    const int again = table.add_wifi(address(1, 2, 3, 4, 9000), 50);
    CHECK(first == again);
    CHECK(table.wifi_count() == 1);
    // the second add does not disturb the stored client
    CHECK(table.wifi_at(first)->last_user_ms == 5);
  }

  {
    TEST("removing a WiFi client frees its slot for a new one");
    multiclient::ClientTable table;
    table.add_wifi(address(1, 1, 1, 1, 1), 0);
    const int b = table.add_wifi(address(2, 2, 2, 2, 2), 0);
    table.add_wifi(address(3, 3, 3, 3, 3), 0);
    CHECK(table.wifi_count() == 3);

    CHECK(table.remove_wifi(b));
    CHECK(table.wifi_count() == 2);
    CHECK(table.find_wifi(address(2, 2, 2, 2, 2)) == -1);

    const int refilled = table.add_wifi(address(4, 4, 4, 4, 4), 0);
    CHECK(refilled == b);
    CHECK(table.wifi_count() == 3);
  }

  {
    TEST("removing an empty or out-of-range slot reports failure");
    multiclient::ClientTable table;
    CHECK(!table.remove_wifi(0));
    CHECK(!table.remove_wifi(-1));
    CHECK(!table.remove_wifi(int(multiclient::max_wifi_clients)));
  }

  {
    TEST("wifi_at exposes fields for the caller to update");
    multiclient::ClientTable table;
    const int index = table.add_wifi(address(9, 9, 9, 9, 42), 100);
    multiclient::Client* client = table.wifi_at(index);
    CHECK(client != nullptr);
    CHECK(client->link == multiclient::Link::wifi);
    CHECK(!client->identified);
    CHECK(client->last_heartbeat_ms == 100);

    client->identified = true;
    client->last_user_ms = 200;
    client->last_heartbeat_ms = 300;

    const multiclient::Client* reread = table.wifi_at(index);
    CHECK(reread->identified);
    CHECK(reread->last_user_ms == 200);
    CHECK(reread->last_heartbeat_ms == 300);
  }

  {
    TEST("wifi_at on an empty or out-of-range slot returns nullptr");
    multiclient::ClientTable table;
    CHECK(table.wifi_at(0) == nullptr);
    CHECK(table.wifi_at(-1) == nullptr);
    CHECK(table.wifi_at(int(multiclient::max_wifi_clients)) == nullptr);
  }

  {
    TEST("clear_wifi empties every slot without touching USB");
    multiclient::ClientTable table;
    table.add_wifi(address(1, 1, 1, 1, 1), 0);
    table.add_wifi(address(2, 2, 2, 2, 2), 0);
    table.set_usb_present(true, 0);

    table.clear_wifi();
    CHECK(table.wifi_count() == 0);
    CHECK(table.usb() != nullptr);
  }

  {
    TEST("USB is a single entry, present or absent, unrelated to the WiFi cap");
    multiclient::ClientTable table;
    table.add_wifi(address(1, 1, 1, 1, 1), 0);
    table.add_wifi(address(2, 2, 2, 2, 2), 0);
    table.add_wifi(address(3, 3, 3, 3, 3), 0);
    CHECK(table.usb() == nullptr);

    table.set_usb_present(true, 10);
    multiclient::Client* usb = table.usb();
    CHECK(usb != nullptr);
    CHECK(usb->link == multiclient::Link::usb);
    CHECK(table.wifi_count() == multiclient::max_wifi_clients);  // unaffected

    // setting present again does not reset an already-connected USB entry
    usb->identified = true;
    table.set_usb_present(true, 20);
    CHECK(table.usb()->identified);

    table.set_usb_present(false, 30);
    CHECK(table.usb() == nullptr);
  }

  {
    TEST("same_address compares every field");
    const multiclient::Address a = address(1, 2, 3, 4, 5);
    const multiclient::Address b = address(1, 2, 3, 4, 5);
    const multiclient::Address different_port = address(1, 2, 3, 4, 6);
    const multiclient::Address different_ip = address(1, 2, 3, 9, 5);
    CHECK(multiclient::same_address(a, b));
    CHECK(!multiclient::same_address(a, different_port));
    CHECK(!multiclient::same_address(a, different_ip));
  }

  {
    TEST("shared_client_table returns the same instance every call");
    multiclient::ClientTable& first = multiclient::shared_client_table();
    multiclient::ClientTable& second = multiclient::shared_client_table();
    CHECK(&first == &second);
  }

  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
