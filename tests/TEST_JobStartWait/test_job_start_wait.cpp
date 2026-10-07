#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "libs/ClientTable.h"
#include "libs/ControlToken.h"
#include "libs/Hello.h"
#include "libs/JobStartWait.h"

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

using multiclient::Client;
using multiclient::ClientTable;
using multiclient::Identity;
using multiclient::JobStartWait;
using multiclient::StartCheck;

constexpr uint32_t second_us = 1000000;

multiclient::Address address(uint8_t last) {
  multiclient::Address a;
  a.ip[0] = 192;
  a.ip[1] = 168;
  a.ip[2] = 1;
  a.ip[3] = last;
  a.port = 5000;
  return a;
}

// Adds a WiFi client and identifies it with `id`, taking part in the wait
// when `takes_part`. Returns its slot index.
int add_identified(ClientTable& table, uint8_t last, uint64_t id, bool takes_part) {
  const int index = table.add_wifi(address(last), 0);
  Client* client = table.wifi_at(index);
  multiclient::set_identity(*client, id, "pc", 2);
  client->features = takes_part ? multiclient::hello_feature_job_start_wait : 0;
  return index;
}

void identify_usb(ClientTable& table, uint64_t id, bool takes_part) {
  table.set_usb_present(true, 0);
  table.start_usb_hello_window(0);
  Client* usb = table.usb();
  multiclient::set_identity(*usb, id, "usb", 3);
  usb->features = takes_part ? multiclient::hello_feature_job_start_wait : 0;
}

Identity identity(const ClientTable& table, int index) { return multiclient::identity_of(table.wifi_at(index)); }

// A ready message's payload for `start_id`: start_id(2, BE).
bool send_ready(const JobStartWait& wait, Client& client, uint16_t start_id) {
  const uint8_t payload[2] = {static_cast<uint8_t>(start_id >> 8), static_cast<uint8_t>(start_id)};
  return wait.mark_ready(client, payload, sizeof(payload));
}

bool refused(const char* line) { return multiclient::refused_while_start_pending(line, std::strlen(line)); }

}  // namespace

int main() {
  {
    TEST("the wait is off until configured, and off again at 0");
    JobStartWait wait;
    CHECK(!wait.enabled());
    CHECK(wait.limit_s() == 0);
    CHECK(wait.hello_ack_features() == 0);
    wait.configure(30);
    CHECK(wait.enabled());
    CHECK(wait.limit_s() == 30);
    CHECK(wait.hello_ack_features() == multiclient::hello_feature_job_start_wait);
    wait.configure(0);
    CHECK(!wait.enabled());
    CHECK(wait.hello_ack_features() == 0);
  }

  {
    TEST("the configured limit is clamped to 0..255 seconds, and defaults to 30");
    JobStartWait wait;
    wait.configure(-5);
    CHECK(wait.limit_s() == 0);
    wait.configure(1000);
    CHECK(wait.limit_s() == 255);
    CHECK(multiclient::default_start_wait_s == 30);
  }

  {
    TEST("needs_wait: only when another identified controller takes part");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    CHECK(!wait.needs_wait(table, identity(table, starter)));  // alone, even though it takes part

    add_identified(table, 2, 0x200, false);  // identified, without the feature
    CHECK(!wait.needs_wait(table, identity(table, starter)));

    table.add_wifi(address(3), 0);  // connected, never identified
    CHECK(!wait.needs_wait(table, identity(table, starter)));

    identify_usb(table, 0x300, true);
    CHECK(wait.needs_wait(table, identity(table, starter)));
  }

  {
    TEST("needs_wait: never while the wait is off, and never for an unidentified starter");
    ClientTable table;
    JobStartWait wait;
    const int starter = add_identified(table, 1, 0x100, false);
    add_identified(table, 2, 0x200, true);
    CHECK(!wait.needs_wait(table, identity(table, starter)));  // not configured
    wait.configure(30);
    CHECK(wait.needs_wait(table, identity(table, starter)));
    CHECK(!wait.needs_wait(table, Identity{}));
  }

  {
    TEST("begin and end: pending, a non-zero start id that changes every start, and the starter");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    CHECK(!wait.pending());
    wait.begin(identity(table, starter), 0);
    CHECK(wait.pending());
    CHECK(wait.start_id() != 0);
    CHECK(wait.starter_id() == 0x100);
    const uint16_t first = wait.start_id();
    wait.end();
    CHECK(!wait.pending());
    wait.begin(identity(table, starter), 0);
    CHECK(wait.start_id() != first);
    wait.end();
  }

  {
    TEST("the start id never takes the value 0 when it wraps");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    bool saw_zero = false;
    for (uint32_t i = 0; i < 70000; ++i) {
      wait.begin(Identity{}, 0);
      if (wait.start_id() == 0) saw_zero = true;
    }
    CHECK(!saw_zero);
  }

  {
    TEST("check: waits for an awaited controller, then all_ready once it sends ready for this start");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    wait.begin(identity(table, starter), 1000);
    CHECK(wait.check(table, 1000) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 1000 + 29 * second_us) == StartCheck::keep_waiting);
    CHECK(send_ready(wait, *table.wifi_at(other), wait.start_id()));
    CHECK(wait.check(table, 2000) == StartCheck::all_ready);
  }

  {
    TEST("check: time_limit once the limit has passed");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    wait.begin(identity(table, starter), 5);
    CHECK(wait.check(table, 5 + 30 * second_us - 1) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 5 + 30 * second_us) == StartCheck::time_limit);
  }

  {
    TEST("check: the limit is measured correctly across a wrap of the microsecond counter");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    const uint32_t start = 0xFFFFFFFFu - 2 * second_us;
    wait.begin(identity(table, starter), start);
    CHECK(wait.check(table, start + 10 * second_us) == StartCheck::keep_waiting);
    CHECK(wait.seconds_left(start + 10 * second_us) == 20);
    CHECK(wait.check(table, start + 30 * second_us) == StartCheck::time_limit);
  }

  {
    TEST("seconds_left counts down in whole seconds, rounded up, and stops at 0");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    wait.begin(Identity{}, 0);
    CHECK(wait.seconds_left(0) == 30);
    CHECK(wait.seconds_left(1) == 30);
    CHECK(wait.seconds_left(second_us) == 29);
    CHECK(wait.seconds_left(29 * second_us + 1) == 1);
    CHECK(wait.seconds_left(30 * second_us) == 0);
    CHECK(wait.seconds_left(40 * second_us) == 0);
  }

  {
    TEST("check: starter_left once the starter is gone from the table, WiFi or USB");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    wait.begin(identity(table, starter), 0);
    table.remove_wifi(starter);
    CHECK(wait.check(table, 0) == StartCheck::starter_left);

    ClientTable usb_table;
    identify_usb(usb_table, 0x300, true);
    add_identified(usb_table, 2, 0x200, true);
    wait.begin(multiclient::identity_of(usb_table.usb()), 0);
    CHECK(wait.check(usb_table, 0) == StartCheck::keep_waiting);
    usb_table.clear_usb_identity();
    CHECK(wait.check(usb_table, 0) == StartCheck::starter_left);
  }

  {
    TEST("check: the starter reconnecting under its own id is not the starter leaving");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    wait.begin(identity(table, starter), 0);
    table.remove_wifi(starter);
    add_identified(table, 9, 0x100, true);
    CHECK(wait.check(table, 0) == StartCheck::keep_waiting);
  }

  {
    TEST("check: an awaited controller that leaves is no longer waited for");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    wait.begin(identity(table, starter), 0);
    table.remove_wifi(other);
    CHECK(wait.check(table, 0) == StartCheck::all_ready);
  }

  {
    TEST("check: a controller that joins during the wait and takes part is waited for too");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    wait.begin(identity(table, starter), 0);
    CHECK(send_ready(wait, *table.wifi_at(other), wait.start_id()));
    const int late = add_identified(table, 3, 0x300, true);
    CHECK(wait.check(table, 0) == StartCheck::keep_waiting);
    CHECK(send_ready(wait, *table.wifi_at(late), wait.start_id()));
    CHECK(wait.check(table, 0) == StartCheck::all_ready);
  }

  {
    TEST("check: controllers without the feature, and unidentified ones, never hold the start");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    add_identified(table, 3, 0x300, false);
    wait.begin(identity(table, starter), 0);
    table.set_usb_present(true, 0);
    table.start_usb_hello_window(0);
    CHECK(send_ready(wait, *table.wifi_at(other), wait.start_id()));
    CHECK(wait.check(table, 0) == StartCheck::all_ready);
  }

  {
    TEST("mark_ready: refused for another start's id, a short payload, an unidentified sender or no wait");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    Client& client = *table.wifi_at(other);
    CHECK(!send_ready(wait, client, 1));  // nothing is waiting
    wait.begin(identity(table, starter), 0);
    CHECK(!send_ready(wait, client, static_cast<uint16_t>(wait.start_id() + 1)));
    const uint8_t short_payload[1] = {0};
    CHECK(!wait.mark_ready(client, short_payload, sizeof(short_payload)));
    CHECK(!wait.mark_ready(client, nullptr, 2));
    const int stranger = table.add_wifi(address(7), 0);
    CHECK(!send_ready(wait, *table.wifi_at(stranger), wait.start_id()));
    CHECK(wait.check(table, 0) == StartCheck::keep_waiting);

    // A ready longer than two bytes is accepted: later fields may follow.
    const uint8_t longer[4] = {static_cast<uint8_t>(wait.start_id() >> 8), static_cast<uint8_t>(wait.start_id()), 9, 9};
    CHECK(wait.mark_ready(client, longer, sizeof(longer)));
    CHECK(wait.check(table, 0) == StartCheck::all_ready);
  }

  {
    TEST("a ready for one start does not count for the next");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    wait.begin(identity(table, starter), 0);
    CHECK(send_ready(wait, *table.wifi_at(other), wait.start_id()));
    wait.end();
    wait.begin(identity(table, starter), 0);
    CHECK(wait.check(table, 0) == StartCheck::keep_waiting);
  }

  {
    TEST("not_ready lists the awaited controllers still loading, WiFi then USB, within capacity");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int ready = add_identified(table, 2, 0x200, true);
    add_identified(table, 3, 0x300, true);
    identify_usb(table, 0x400, true);
    wait.begin(identity(table, starter), 0);
    CHECK(send_ready(wait, *table.wifi_at(ready), wait.start_id()));
    uint64_t ids[multiclient::max_wifi_clients + 1] = {};
    CHECK(wait.not_ready(table, ids, sizeof(ids) / sizeof(ids[0])) == 2);
    CHECK(ids[0] == 0x300);
    CHECK(ids[1] == 0x400);
    CHECK(wait.not_ready(table, ids, 1) == 1);
    CHECK(ids[0] == 0x300);
  }

  {
    TEST("refused_while_start_pending: G-code, M-code, tool, homing, jog, and the commands that act on a job");
    CHECK(refused("G0 X10"));
    CHECK(refused("  G1 X1 F100"));
    CHECK(refused("M3 S10000"));
    CHECK(refused("M32 part.nc"));
    CHECK(refused("M6T2"));
    CHECK(refused("T1"));
    CHECK(refused("$H"));
    CHECK(refused("$J X10 F500"));
    CHECK(refused("$J -c X"));
    CHECK(refused("play /sd/gcodes/part.nc"));
    CHECK(refused("play"));
    CHECK(refused("buffer M495 X0Y0P1"));
    CHECK(refused("goto 100"));
    CHECK(refused("suspend 5"));
    CHECK(refused("resume"));
  }

  {
    TEST("refused_while_start_pending: start-now, abort, reads, transfers and overrides still run");
    CHECK(!refused("start-now"));
    CHECK(!refused("abort"));
    CHECK(!refused("ls -e -s /sd/gcodes"));
    CHECK(!refused("config-get sd wifi.max_clients"));
    CHECK(!refused("download /sd/gcodes/part.nc"));
    CHECK(!refused("md5sum /sd/gcodes/part.nc"));
    CHECK(!refused("progress"));
    CHECK(!refused("player"));
    CHECK(!refused("$X"));
    CHECK(!refused("$G"));
    CHECK(!refused("$F S120"));
    CHECK(!refused(""));
    CHECK(!refused("   "));
    CHECK(!multiclient::refused_while_start_pending(nullptr, 0));
    CHECK(!multiclient::refused_while_start_pending("G0", 0));
  }

  {
    TEST("job_start_pending_reply: one line in the control gate's refusal form");
    const char* reply = multiclient::job_start_pending_reply;
    const std::size_t length = std::strlen(reply);
    const char prefix[] = "error:Refused -- ";
    CHECK(std::strncmp(reply, prefix, sizeof(prefix) - 1) == 0);
    CHECK(std::strcmp(reply + length - 2, "\r\n") == 0);
    CHECK(std::strpbrk(reply, "\r\n") == reply + length - 2);
  }

  {
    TEST("shared_job_start_wait returns the same object every time");
    CHECK(&multiclient::shared_job_start_wait() == &multiclient::shared_job_start_wait());
  }

  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
