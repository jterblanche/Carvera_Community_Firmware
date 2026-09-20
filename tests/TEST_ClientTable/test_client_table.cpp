#include <cstdint>
#include <cstdio>
#include <string>

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

  {
    TEST("set_identity records id and name and clamps an over-length name");
    multiclient::Client client;
    multiclient::set_identity(client, 0x0102030405060708ULL, "Office PC", 9);
    CHECK(client.identified);
    CHECK(client.id == 0x0102030405060708ULL);
    CHECK(client.name_len == 9);
    CHECK(std::string(client.name) == "Office PC");

    multiclient::Client clamped;
    const char too_long[41] = "0123456789012345678901234567890123456789";
    multiclient::set_identity(clamped, 1, too_long, 40);
    CHECK(clamped.name_len == multiclient::max_name_length);
  }

  {
    TEST("a client is not old before its hello window starts");
    multiclient::Client client;  // hello_window_started defaults to false
    CHECK(!multiclient::client_is_old(client, 1'000'000));
  }

  {
    TEST("a client is not old before its hello window has elapsed");
    multiclient::Client client;
    client.hello_window_started = true;
    client.hello_window_start_ms = 1000;
    CHECK(!multiclient::client_is_old(client, 1000));
    CHECK(!multiclient::client_is_old(client, 1000 + multiclient::hello_window_ms - 1));
  }

  {
    TEST("a client is old once its hello window has elapsed without identifying");
    multiclient::Client client;
    client.hello_window_started = true;
    client.hello_window_start_ms = 1000;
    CHECK(multiclient::client_is_old(client, 1000 + multiclient::hello_window_ms));
    CHECK(multiclient::client_is_old(client, 1000 + multiclient::hello_window_ms + 60'000));
  }

  {
    TEST("an identified client is never old, however long its window has run");
    multiclient::Client client;
    client.hello_window_started = true;
    client.hello_window_start_ms = 1000;
    client.identified = true;
    CHECK(!multiclient::client_is_old(client, 1000 + multiclient::hello_window_ms + 60'000));
  }

  {
    TEST("a WiFi client's hello window starts the moment it is admitted");
    multiclient::ClientTable table;
    const int index = table.add_wifi(address(1, 2, 3, 4, 1), 5000);
    const multiclient::Client* client = table.wifi_at(index);
    CHECK(client->hello_window_started);
    CHECK(client->hello_window_start_ms == 5000);
    CHECK(!multiclient::client_is_old(*client, 5000 + multiclient::hello_window_ms - 1));
    CHECK(multiclient::client_is_old(*client, 5000 + multiclient::hello_window_ms));
  }

  {
    TEST("a USB entry's hello window does not start until start_usb_hello_window is called");
    multiclient::ClientTable table;
    table.set_usb_present(true, 0);  // present from boot, nothing sent yet
    const multiclient::Client* usb = table.usb();
    CHECK(!usb->hello_window_started);
    CHECK(!multiclient::client_is_old(*usb, 1'000'000));  // never old while idle, however long

    table.start_usb_hello_window(2000);
    CHECK(table.usb()->hello_window_started);
    CHECK(table.usb()->hello_window_start_ms == 2000);
    CHECK(multiclient::client_is_old(*table.usb(), 2000 + multiclient::hello_window_ms));

    // Calling it again once started does not push the deadline back.
    table.start_usb_hello_window(9000);
    CHECK(table.usb()->hello_window_start_ms == 2000);
  }

  {
    TEST("clear_usb_identity resets identity and the hello window but keeps the slot");
    multiclient::ClientTable table;
    table.set_usb_present(true, 0);
    table.start_usb_hello_window(100);
    multiclient::set_identity(*table.usb(), 42, "USB", 3);

    table.clear_usb_identity();
    const multiclient::Client* usb = table.usb();
    CHECK(usb != nullptr);
    CHECK(usb->link == multiclient::Link::usb);
    CHECK(!usb->identified);
    CHECK(!usb->hello_window_started);
  }

  {
    TEST("present_count excludes an idle USB entry but includes a talking one");
    multiclient::ClientTable table;
    CHECK(table.present_count() == 0);

    table.set_usb_present(true, 0);
    CHECK(table.present_count() == 0);  // present, but never talked

    table.start_usb_hello_window(10);
    CHECK(table.present_count() == 1);

    table.add_wifi(address(1, 1, 1, 1, 1), 0);
    CHECK(table.present_count() == 2);
  }

  {
    TEST("a lone unidentified WiFi client, past its window, is not old-and-not-alone");
    multiclient::ClientTable table;
    const int index = table.add_wifi(address(1, 1, 1, 1, 1), 0);
    const uint32_t later = multiclient::hello_window_ms;
    CHECK(multiclient::client_is_old(*table.wifi_at(index), later));
    CHECK(table.present_count() == 1);  // alone: today's behaviour, nothing to enforce
  }

  {
    TEST("a second unidentified client makes the first one not-alone");
    multiclient::ClientTable table;
    const int first = table.add_wifi(address(1, 1, 1, 1, 1), 0);
    table.add_wifi(address(2, 2, 2, 2, 2), 0);
    const uint32_t later = multiclient::hello_window_ms;
    CHECK(multiclient::client_is_old(*table.wifi_at(first), later));
    CHECK(table.present_count() == 2);  // not alone
  }

  {
    TEST("has_old_client excludes the given WiFi index and, optionally, USB");
    multiclient::ClientTable table;
    const int old_index = table.add_wifi(address(1, 1, 1, 1, 1), 0);
    const uint32_t later = multiclient::hello_window_ms;

    CHECK(table.has_old_client(later));              // the WiFi entry is old
    CHECK(!table.has_old_client(later, old_index));  // excluded by index, nothing else present

    table.set_usb_present(true, 0);
    table.start_usb_hello_window(0);
    CHECK(table.has_old_client(later, old_index));                         // USB is old too, not excluded by default
    CHECK(!table.has_old_client(later, old_index, /*exclude_usb=*/true));  // both excluded now

    table.remove_wifi(old_index);
    CHECK(table.has_old_client(later));                          // only USB left, and it's old
    CHECK(!table.has_old_client(later, -1, /*exclude_usb=*/true));  // USB excluded too: nobody old left
  }

  {
    TEST("find_wifi_by_id only matches an identified client, excluding a given index");
    multiclient::ClientTable table;
    const int a = table.add_wifi(address(1, 1, 1, 1, 1), 0);
    const int b = table.add_wifi(address(2, 2, 2, 2, 2), 0);
    CHECK(table.find_wifi_by_id(42) == -1);  // nobody identified yet

    multiclient::set_identity(*table.wifi_at(a), 42, "A", 1);
    CHECK(table.find_wifi_by_id(42) == a);
    CHECK(table.find_wifi_by_id(42, a) == -1);  // excluded
    CHECK(table.find_wifi_by_id(99) == -1);

    multiclient::set_identity(*table.wifi_at(b), 99, "B", 1);
    CHECK(table.find_wifi_by_id(99) == b);
  }

  {
    TEST("usb_has_id only matches an identified USB entry holding that id");
    multiclient::ClientTable table;
    CHECK(!table.usb_has_id(7));
    table.set_usb_present(true, 0);
    CHECK(!table.usb_has_id(7));  // present, not identified
    multiclient::set_identity(*table.usb(), 7, "USB", 3);
    CHECK(table.usb_has_id(7));
    CHECK(!table.usb_has_id(8));
  }

  {
    TEST("usb_session_expired is false until the window has started");
    CHECK(!multiclient::usb_session_expired(false, 1'000'000, 0));
  }

  {
    TEST("usb_session_expired is false before the idle timeout has elapsed");
    CHECK(!multiclient::usb_session_expired(true, 1000, 1000));
    CHECK(!multiclient::usb_session_expired(true, 1000 + multiclient::usb_idle_timeout_ms - 1, 1000));
  }

  {
    TEST("usb_session_expired is true once the idle timeout has elapsed");
    CHECK(multiclient::usb_session_expired(true, 1000 + multiclient::usb_idle_timeout_ms, 1000));
    CHECK(multiclient::usb_session_expired(true, 1000 + multiclient::usb_idle_timeout_ms + 60'000, 1000));
  }

  {
    TEST("usb_session_expired is false when last_activity_ms is ahead of now_ms");
    // The caller samples now_ms, then reads last_activity_ms as a second
    // step; a byte can arrive on the interrupt in between, leaving
    // last_activity_ms momentarily ahead of the already-sampled now_ms.
    // Plain unsigned subtraction would wrap to a huge value here and
    // wrongly report "expired" on an actively-talking link.
    CHECK(!multiclient::usb_session_expired(true, 1000, 1001));
  }

  {
    TEST("a USB session that goes quiet for the idle timeout is reset, then can restart");
    multiclient::ClientTable table;
    table.set_usb_present(true, 0);
    table.start_usb_hello_window(0);
    CHECK(table.present_count() == 1);

    // Still short of the idle timeout: nothing to do yet (this is what the
    // caller checks before calling clear_usb_identity()).
    CHECK(!multiclient::usb_session_expired(table.usb()->hello_window_started, multiclient::usb_idle_timeout_ms - 1, 0));

    // At the idle timeout, the caller resets the session.
    CHECK(multiclient::usb_session_expired(table.usb()->hello_window_started, multiclient::usb_idle_timeout_ms, 0));
    table.clear_usb_identity();
    CHECK(table.present_count() == 0);  // no longer counts

    // A byte arriving later starts a fresh window.
    table.start_usb_hello_window(50'000);
    CHECK(table.usb()->hello_window_start_ms == 50'000);
    CHECK(table.present_count() == 1);
  }

  {
    TEST("any_identified_present is false when nobody present has identified");
    multiclient::ClientTable table;
    CHECK(!table.any_identified_present());  // nobody connected

    const int a = table.add_wifi(address(1, 1, 1, 1, 1), 0);
    CHECK(!table.any_identified_present());  // unidentified, whether old or not

    table.add_wifi(address(2, 2, 2, 2, 2), 0);
    CHECK(!table.any_identified_present());  // still nobody identified

    (void)a;
  }

  {
    TEST("any_identified_present is true once any present WiFi client identifies");
    multiclient::ClientTable table;
    const int a = table.add_wifi(address(1, 1, 1, 1, 1), 0);
    table.add_wifi(address(2, 2, 2, 2, 2), 0);  // left unidentified, and old later
    CHECK(!table.any_identified_present());

    multiclient::set_identity(*table.wifi_at(a), 1, "A", 1);
    CHECK(table.any_identified_present());
  }

  {
    TEST("any_identified_present accounts for an identified USB entry too");
    multiclient::ClientTable table;
    table.set_usb_present(true, 0);
    table.start_usb_hello_window(0);
    CHECK(!table.any_identified_present());

    multiclient::set_identity(*table.usb(), 9, "USB", 3);
    CHECK(table.any_identified_present());
  }

  {
    TEST("is_earliest_present picks the lowest hello_window_start_ms, WiFi or USB");
    multiclient::ClientTable table;
    const int a = table.add_wifi(address(1, 1, 1, 1, 1), 1000);
    const int b = table.add_wifi(address(2, 2, 2, 2, 2), 2000);
    CHECK(table.is_earliest_present(a));
    CHECK(!table.is_earliest_present(b));

    table.set_usb_present(true, 0);
    table.start_usb_hello_window(500);  // earlier than both WiFi clients
    CHECK(!table.is_earliest_present(a));
    CHECK(!table.is_earliest_present(b));
  }

  {
    TEST("is_earliest_present is false for an out-of-range or empty slot");
    multiclient::ClientTable table;
    table.add_wifi(address(1, 1, 1, 1, 1), 0);
    CHECK(!table.is_earliest_present(1));   // empty slot
    CHECK(!table.is_earliest_present(-1));  // out of range
    CHECK(!table.is_earliest_present(int(multiclient::max_wifi_clients)));
  }

  {
    TEST("two old clients a few seconds apart: the later one is old and not spared");
    // This mirrors what enforce_old_client_rule() in WifiProvider.cpp does:
    // spare_earliest = !table.any_identified_present(); then, for every
    // client that is old, skip it if it is the earliest present one.
    multiclient::ClientTable table;
    const int first = table.add_wifi(address(1, 1, 1, 1, 1), 0);
    const int second = table.add_wifi(address(2, 2, 2, 2, 2), 3000);  // 3s later

    // At 5s, first is old; second (only 2s into its own window) is not --
    // not yet decided, not a reason to disconnect anyone.
    const uint32_t at_5s = 5000;
    CHECK(multiclient::client_is_old(*table.wifi_at(first), at_5s));
    CHECK(!multiclient::client_is_old(*table.wifi_at(second), at_5s));
    CHECK(!table.any_identified_present());
    CHECK(table.is_earliest_present(first));  // spared regardless -- it's the earliest present

    // At 8s, second's own window has also run out: now both are old, but
    // only the later one (second) is fair game -- first stays spared as
    // the earliest, for as long as it remains present, whether or not it
    // has itself been decided old.
    const uint32_t at_8s = 8000;
    CHECK(multiclient::client_is_old(*table.wifi_at(first), at_8s));
    CHECK(multiclient::client_is_old(*table.wifi_at(second), at_8s));
    CHECK(!table.any_identified_present());
    CHECK(table.is_earliest_present(first));
    CHECK(!table.is_earliest_present(second));
    // enforce_old_client_rule() would disconnect second and leave first.
  }

  {
    TEST("once the earliest disconnects, the next-earliest present becomes the keeper");
    multiclient::ClientTable table;
    const int first = table.add_wifi(address(1, 1, 1, 1, 1), 0);
    table.add_wifi(address(2, 2, 2, 2, 2), 3000);
    const int third = table.add_wifi(address(3, 3, 3, 3, 3), 6000);

    CHECK(table.is_earliest_present(first));
    table.remove_wifi(first);  // as if enforce_old_client_rule()'s driver disconnect took

    // second is now the earliest of what remains.
    const int second_index = table.find_wifi(address(2, 2, 2, 2, 2));
    CHECK(table.is_earliest_present(second_index));
    CHECK(!table.is_earliest_present(third));
  }

  {
    TEST("a client that identifies ends its own candidacy, and everyone else's exemption");
    multiclient::ClientTable table;
    const int first = table.add_wifi(address(1, 1, 1, 1, 1), 0);
    table.add_wifi(address(2, 2, 2, 2, 2), 3000);
    const uint32_t now = 8000;  // both old, per the earlier scenario

    CHECK(!table.any_identified_present());
    CHECK(multiclient::client_is_old(*table.wifi_at(first), now));

    // The spared, earliest client identifies (a late hello, still
    // accepted -- has_old_client() excludes its own index, so nothing
    // stops this).
    multiclient::set_identity(*table.wifi_at(first), 42, "First", 5);
    CHECK(!multiclient::client_is_old(*table.wifi_at(first), now));  // no longer a candidate itself
    CHECK(table.any_identified_present());  // and the exemption stops applying to anyone else
  }

  {
    TEST("record_heartbeat updates last_heartbeat_ms and nothing else");
    multiclient::ClientTable table;
    const int index = table.add_wifi(address(1, 1, 1, 1, 1), 100);
    multiclient::Client *client = table.wifi_at(index);
    CHECK(client->last_heartbeat_ms == 100);  // set by add_wifi
    multiclient::record_heartbeat(*client, 5000);
    CHECK(client->last_heartbeat_ms == 5000);
    CHECK(!client->identified);  // a heartbeat never identifies a client
  }

  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
