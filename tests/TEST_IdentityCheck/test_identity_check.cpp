#include <cstdint>
#include <cstdio>
#include <cstring>

#include "libs/ClientTable.h"
#include "libs/ControlToken.h"
#include "libs/Hello.h"
#include "libs/IdentityCheck.h"

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

using multiclient::CheckStep;
using multiclient::Client;
using multiclient::ClientTable;
using multiclient::ControlToken;
using multiclient::Hello;
using multiclient::HelloAction;
using multiclient::IdentityCheck;
using multiclient::Link;
using multiclient::Seat;

constexpr uint32_t ms = 1000;
constexpr uint64_t office_id = 0x1122334455667788ULL;

multiclient::Address address(uint8_t last) {
  multiclient::Address a;
  a.ip[0] = 192;
  a.ip[1] = 168;
  a.ip[2] = 1;
  a.ip[3] = last;
  a.port = 40000 + last;
  return a;
}

Hello make_hello(uint64_t id, bool has_launch, uint64_t launch, const char* name = "Office PC") {
  Hello hello;
  hello.id = id;
  hello.name_len = static_cast<uint8_t>(std::strlen(name));
  std::memcpy(hello.name, name, hello.name_len);
  hello.has_launch = has_launch;
  hello.launch = launch;
  return hello;
}

// Adds a WiFi client admitted at `now_us` and identifies it with `hello`.
int add_identified(ClientTable& table, uint8_t last, const Hello& hello, uint32_t now_us = 0) {
  const int index = table.add_wifi(address(last), now_us);
  ControlToken unused;
  multiclient::admit_hello(*table.wifi_at(index), hello, unused);
  return index;
}

void identify_usb(ClientTable& table, const Hello& hello) {
  table.set_usb_present(true, 0);
  table.start_usb_hello_window(0);
  ControlToken unused;
  multiclient::admit_hello(*table.usb(), hello, unused);
}

void start_usb(ClientTable& table, uint32_t now_us = 0) {
  table.set_usb_present(true, now_us);
  table.start_usb_hello_window(now_us);
}

// Gives `holder` control the way a user-caused command does in single-user
// mode.
void take_control(ControlToken& control, const Client* holder) {
  multiclient::MotionState idle;
  idle.idle = true;
  control.gate(multiclient::identity_of(holder), multiclient::Traffic::user_caused, idle);
}

uint32_t question_number(const IdentityCheck& check) {
  uint8_t q[multiclient::presence_check_length];
  CHECK(check.build_question(q) == multiclient::presence_check_length);
  return (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | q[3];
}

bool answer(IdentityCheck& check, const Seat& from, uint32_t number) {
  const uint8_t payload[4] = {static_cast<uint8_t>(number >> 24), static_cast<uint8_t>(number >> 16),
                              static_cast<uint8_t>(number >> 8), static_cast<uint8_t>(number)};
  return check.note_answer(from, payload, sizeof(payload));
}

// Polls `link` every 100 ms from `from_us` up to and including `to_us`,
// returning the first step that is not wait (or wait if none).
CheckStep poll_until(IdentityCheck& check, const ClientTable& table, Link link, uint32_t from_us, uint32_t to_us,
                     uint32_t* at_us = nullptr) {
  for (uint32_t t = from_us; static_cast<int32_t>(to_us - t) >= 0; t += 100 * ms) {
    const CheckStep step = check.next_step(table, link, t);
    if (step != CheckStep::wait) {
      if (at_us != nullptr) *at_us = t;
      return step;
    }
  }
  return CheckStep::wait;
}

}  // namespace

int main() {
  {
    TEST("same_launch: both launch parts equal, or neither sent");
    Client client;
    client.has_launch = true;
    client.launch = 42;
    CHECK(multiclient::same_launch(client, make_hello(1, true, 42)));
    CHECK(!multiclient::same_launch(client, make_hello(1, true, 43)));
    CHECK(!multiclient::same_launch(client, make_hello(1, false, 0)));
    client.has_launch = false;
    client.launch = 0;
    CHECK(multiclient::same_launch(client, make_hello(1, false, 0)));
    CHECK(!multiclient::same_launch(client, make_hello(1, true, 0)));
  }

  {
    TEST("decide_hello: an id nobody else has is admitted");
    ClientTable table;
    add_identified(table, 1, make_hello(7, true, 1));
    const int self = table.add_wifi(address(2), 0);
    const auto decision = multiclient::decide_hello(table, multiclient::wifi_seat(self), make_hello(office_id, true, 1));
    CHECK(decision.action == HelloAction::admit);
  }

  {
    TEST("decide_hello: the same id from the same launch on another WiFi slot is replaced");
    ClientTable table;
    const int old = add_identified(table, 1, make_hello(office_id, true, 99));
    const int self = table.add_wifi(address(2), 0);
    const auto decision = multiclient::decide_hello(table, multiclient::wifi_seat(self), make_hello(office_id, true, 99));
    CHECK(decision.action == HelloAction::replace);
    CHECK(decision.other.link == Link::wifi);
    CHECK(decision.other.index == old);
  }

  {
    TEST("decide_hello: neither sending a launch part is the same launch");
    ClientTable table;
    add_identified(table, 1, make_hello(office_id, false, 0));
    const int self = table.add_wifi(address(2), 0);
    CHECK(multiclient::decide_hello(table, multiclient::wifi_seat(self), make_hello(office_id, false, 0)).action ==
          HelloAction::replace);
  }

  {
    TEST("decide_hello: a different launch, or only one side sending one, is asked first");
    ClientTable table;
    const int old = add_identified(table, 1, make_hello(office_id, true, 99));
    const int self = table.add_wifi(address(2), 0);
    const auto decision = multiclient::decide_hello(table, multiclient::wifi_seat(self), make_hello(office_id, true, 100));
    CHECK(decision.action == HelloAction::ask);
    CHECK(decision.other.index == old);
    CHECK(multiclient::decide_hello(table, multiclient::wifi_seat(self), make_hello(office_id, false, 0)).action ==
          HelloAction::ask);

    ClientTable old_style;
    add_identified(old_style, 1, make_hello(office_id, false, 0));
    const int newer = old_style.add_wifi(address(2), 0);
    CHECK(multiclient::decide_hello(old_style, multiclient::wifi_seat(newer), make_hello(office_id, true, 5)).action ==
          HelloAction::ask);
  }

  {
    TEST("decide_hello: a WiFi hello finds the id on USB, and a USB hello finds it on WiFi");
    ClientTable table;
    identify_usb(table, make_hello(office_id, true, 1));
    const int self = table.add_wifi(address(2), 0);
    auto decision = multiclient::decide_hello(table, multiclient::wifi_seat(self), make_hello(office_id, true, 2));
    CHECK(decision.action == HelloAction::ask);
    CHECK(decision.other.link == Link::usb);

    ClientTable other;
    const int wifi = add_identified(other, 1, make_hello(office_id, true, 1));
    start_usb(other);
    decision = multiclient::decide_hello(other, multiclient::usb_seat(), make_hello(office_id, true, 1));
    CHECK(decision.action == HelloAction::replace);
    CHECK(decision.other.link == Link::wifi);
    CHECK(decision.other.index == wifi);
  }

  {
    TEST("decide_hello: unidentified entries and the sender itself are not matches");
    ClientTable table;
    const int unidentified = table.add_wifi(address(1), 0);
    table.wifi_at(unidentified)->id = office_id;  // never said hello: id unused
    const int self = table.add_wifi(address(2), 0);
    CHECK(multiclient::decide_hello(table, multiclient::wifi_seat(self), make_hello(office_id, true, 1)).action ==
          HelloAction::admit);

    start_usb(table);
    table.usb()->id = office_id;
    CHECK(multiclient::decide_hello(table, multiclient::usb_seat(), make_hello(office_id, true, 1)).action ==
          HelloAction::admit);
  }

  {
    TEST("admit_hello records the identity, features and launch part");
    ClientTable table;
    const int index = table.add_wifi(address(1), 0);
    ControlToken control;
    Hello hello = make_hello(office_id, true, 0xA1B2C3D4E5F60718ULL);
    hello.features = multiclient::hello_feature_job_start_wait;
    CHECK(!multiclient::admit_hello(*table.wifi_at(index), hello, control));
    const Client* client = table.wifi_at(index);
    CHECK(client->identified);
    CHECK(client->id == office_id);
    CHECK(client->name_len == 9);
    CHECK(std::strcmp(client->name, "Office PC") == 0);
    CHECK(client->features == multiclient::hello_feature_job_start_wait);
    CHECK(client->has_launch);
    CHECK(client->launch == 0xA1B2C3D4E5F60718ULL);
  }

  {
    TEST("a reconnect under the id holding control frees control");
    ClientTable table;
    ControlToken control;
    const int old = add_identified(table, 1, make_hello(office_id, true, 5));
    take_control(control, table.wifi_at(old));
    CHECK(control.has_holder());

    // The same launch on a new socket: the old entry is removed and the
    // newcomer admitted in the same pass, so the id never leaves the
    // table and reconcile_holder() alone would keep control with it.
    const int self = table.add_wifi(address(2), 0);
    table.remove_wifi(old);
    CHECK(multiclient::admit_hello(*table.wifi_at(self), make_hello(office_id, true, 5), control));
    CHECK(!control.has_holder());
    CHECK(!multiclient::reconcile_holder(control, table));
  }

  {
    TEST("admitting a new id leaves another controller's control alone");
    ClientTable table;
    ControlToken control;
    const int holder = add_identified(table, 1, make_hello(7, true, 5));
    take_control(control, table.wifi_at(holder));
    const int self = table.add_wifi(address(2), 0);
    CHECK(!multiclient::admit_hello(*table.wifi_at(self), make_hello(office_id, true, 5), control));
    CHECK(control.has_holder());
    CHECK(control.holder().id == 7);
  }

  {
    TEST("begin holds the newcomer, and a second check cannot start");
    ClientTable table;
    IdentityCheck check;
    const int old = add_identified(table, 1, make_hello(office_id, true, 1));
    const int self = table.add_wifi(address(2), 0);
    const int third = table.add_wifi(address(3), 0);
    CHECK(!check.active());
    CHECK(check.begin(table, multiclient::wifi_seat(self), multiclient::wifi_seat(old), make_hello(office_id, true, 2),
                      true, 0));
    CHECK(check.active());
    CHECK(check.holds(multiclient::wifi_seat(self)));
    CHECK(!check.holds(multiclient::wifi_seat(old)));
    CHECK(!check.holds(multiclient::usb_seat()));
    CHECK(table.wifi_at(self)->hello_held);
    CHECK(!check.begin(table, multiclient::wifi_seat(third), multiclient::wifi_seat(old),
                       make_hello(office_id, true, 3), true, 0));
    CHECK(!table.wifi_at(third)->hello_held);
    CHECK(check.newcomer().index == self);
    CHECK(check.hello().launch == 2);
  }

  {
    TEST("the connected controller answers: the newcomer is refused and the old one stays");
    ClientTable table;
    IdentityCheck check;
    const int old = add_identified(table, 1, make_hello(office_id, true, 1));
    const int self = table.add_wifi(address(2), 0);
    CHECK(check.begin(table, multiclient::wifi_seat(self), multiclient::wifi_seat(old), make_hello(office_id, true, 2),
                      true, 10 * ms));
    CHECK(check.next_step(table, Link::usb, 10 * ms) == CheckStep::wait);
    CHECK(check.next_step(table, Link::wifi, 10 * ms) == CheckStep::ask);
    const uint32_t number = question_number(check);
    check.asked(10 * ms);
    CHECK(check.next_step(table, Link::wifi, 20 * ms) == CheckStep::wait);

    CHECK(answer(check, multiclient::wifi_seat(old), number));
    CHECK(check.next_step(table, Link::usb, 80 * ms) == CheckStep::wait);
    CHECK(check.next_step(table, Link::wifi, 80 * ms) == CheckStep::refuse);
    // Still refused even if the answer arrived just before the time ran out.
    CHECK(check.next_step(table, Link::wifi, 5000 * ms) == CheckStep::refuse);
    check.end(table);
    CHECK(!check.active());
    CHECK(!table.wifi_at(self)->hello_held);
    CHECK(!table.wifi_at(self)->identified);
    CHECK(table.wifi_at(old)->identified);
    CHECK(check.next_step(table, Link::wifi, 90 * ms) == CheckStep::wait);
  }

  {
    TEST("no answer within 2 s: the old entry is retired, then the newcomer admitted");
    ClientTable table;
    IdentityCheck check;
    const int old = add_identified(table, 1, make_hello(office_id, true, 1));
    const int self = table.add_wifi(address(2), 0);
    CHECK(check.begin(table, multiclient::wifi_seat(self), multiclient::wifi_seat(old), make_hello(office_id, true, 2),
                      true, 0));
    CHECK(check.next_step(table, Link::wifi, 0) == CheckStep::ask);
    check.asked(0);
    uint32_t at = 0;
    CHECK(poll_until(check, table, Link::wifi, 100 * ms, 1900 * ms) == CheckStep::wait);
    CHECK(poll_until(check, table, Link::wifi, 2000 * ms, 2000 * ms, &at) == CheckStep::retire);
    CHECK(at == 2000 * ms);
    // Repeats until the link has done it.
    CHECK(check.next_step(table, Link::wifi, 2001 * ms) == CheckStep::retire);
    table.remove_wifi(old);
    check.retired();
    CHECK(check.next_step(table, Link::wifi, 2002 * ms) == CheckStep::admit);
    CHECK(check.newcomer().index == self);
    check.end(table);
    CHECK(!table.wifi_at(self)->hello_held);
  }

  {
    TEST("an answer that arrives after the time ran out does not count");
    ClientTable table;
    IdentityCheck check;
    const int old = add_identified(table, 1, make_hello(office_id, true, 1));
    const int self = table.add_wifi(address(2), 0);
    check.begin(table, multiclient::wifi_seat(self), multiclient::wifi_seat(old), make_hello(office_id, true, 2), true, 0);
    check.next_step(table, Link::wifi, 0);
    const uint32_t number = question_number(check);
    check.asked(0);
    CHECK(poll_until(check, table, Link::wifi, 100 * ms, 2000 * ms) == CheckStep::retire);
    CHECK(!answer(check, multiclient::wifi_seat(old), number));
    CHECK(check.next_step(table, Link::wifi, 2050 * ms) == CheckStep::retire);
  }

  {
    TEST("answers from elsewhere, with another number, too short, or before asking do not count");
    ClientTable table;
    IdentityCheck check;
    const int old = add_identified(table, 1, make_hello(office_id, true, 1));
    const int self = table.add_wifi(address(2), 0);
    const int other = add_identified(table, 3, make_hello(7, true, 1));
    check.begin(table, multiclient::wifi_seat(self), multiclient::wifi_seat(old), make_hello(office_id, true, 2), true, 0);
    const uint32_t number = question_number(check);
    CHECK(!answer(check, multiclient::wifi_seat(old), number));  // not asked yet
    CHECK(check.next_step(table, Link::wifi, 0) == CheckStep::ask);
    check.asked(0);
    CHECK(!answer(check, multiclient::wifi_seat(other), number));
    CHECK(!answer(check, multiclient::wifi_seat(self), number));
    CHECK(!answer(check, multiclient::usb_seat(), number));
    CHECK(!answer(check, multiclient::wifi_seat(old), number + 1));
    const uint8_t short_payload[3] = {0, 0, 0};
    CHECK(!check.note_answer(multiclient::wifi_seat(old), short_payload, sizeof(short_payload)));
    CHECK(!check.note_answer(multiclient::wifi_seat(old), nullptr, 4));
    CHECK(check.next_step(table, Link::wifi, 100 * ms) == CheckStep::wait);
    // Bytes after the number are ignored.
    const uint8_t longer[6] = {static_cast<uint8_t>(number >> 24), static_cast<uint8_t>(number >> 16),
                               static_cast<uint8_t>(number >> 8), static_cast<uint8_t>(number), 0xEE, 0xEE};
    CHECK(check.note_answer(multiclient::wifi_seat(old), longer, sizeof(longer)));
    CHECK(check.next_step(table, Link::wifi, 150 * ms) == CheckStep::refuse);
  }

  {
    TEST("an answer to one check never counts for the next");
    ClientTable table;
    IdentityCheck check;
    const int old = add_identified(table, 1, make_hello(office_id, true, 1));
    const int self = table.add_wifi(address(2), 0);
    check.begin(table, multiclient::wifi_seat(self), multiclient::wifi_seat(old), make_hello(office_id, true, 2), true, 0);
    check.next_step(table, Link::wifi, 0);
    const uint32_t first = question_number(check);
    check.asked(0);
    CHECK(answer(check, multiclient::wifi_seat(old), first));
    check.end(table);

    check.begin(table, multiclient::wifi_seat(self), multiclient::wifi_seat(old), make_hello(office_id, true, 3), true,
                100 * ms);
    CHECK(question_number(check) != first);
    check.next_step(table, Link::wifi, 100 * ms);
    check.asked(100 * ms);
    CHECK(!answer(check, multiclient::wifi_seat(old), first));
  }

  {
    TEST("WiFi newcomer, USB old: USB asks and retires, WiFi admits");
    ClientTable table;
    IdentityCheck check;
    identify_usb(table, make_hello(office_id, true, 1));
    const int self = table.add_wifi(address(2), 0);
    CHECK(check.begin(table, multiclient::wifi_seat(self), multiclient::usb_seat(), make_hello(office_id, true, 2), true, 0));
    CHECK(check.next_step(table, Link::wifi, 0) == CheckStep::wait);
    CHECK(check.next_step(table, Link::usb, 0) == CheckStep::ask);
    check.asked(0);
    CHECK(poll_until(check, table, Link::wifi, 100 * ms, 2500 * ms) == CheckStep::wait);
    CHECK(check.next_step(table, Link::usb, 2500 * ms) == CheckStep::retire);
    table.clear_usb_identity();
    check.retired();
    CHECK(check.next_step(table, Link::usb, 2501 * ms) == CheckStep::wait);
    CHECK(check.next_step(table, Link::wifi, 2501 * ms) == CheckStep::admit);
  }

  {
    TEST("WiFi newcomer, USB old that answers: the WiFi link refuses");
    ClientTable table;
    IdentityCheck check;
    identify_usb(table, make_hello(office_id, true, 1));
    const int self = table.add_wifi(address(2), 0);
    check.begin(table, multiclient::wifi_seat(self), multiclient::usb_seat(), make_hello(office_id, true, 2), true, 0);
    CHECK(check.next_step(table, Link::usb, 0) == CheckStep::ask);
    check.asked(0);
    CHECK(answer(check, multiclient::usb_seat(), question_number(check)));
    CHECK(check.next_step(table, Link::usb, 50 * ms) == CheckStep::wait);
    CHECK(check.next_step(table, Link::wifi, 50 * ms) == CheckStep::refuse);
  }

  {
    TEST("USB newcomer, WiFi old from the same launch: WiFi retires at once, USB admits");
    ClientTable table;
    IdentityCheck check;
    const int old = add_identified(table, 1, make_hello(office_id, true, 1));
    start_usb(table);
    CHECK(check.begin(table, multiclient::usb_seat(), multiclient::wifi_seat(old), make_hello(office_id, true, 1), false,
                      0));
    CHECK(table.usb()->hello_held);
    CHECK(check.next_step(table, Link::usb, 0) == CheckStep::wait);
    CHECK(check.next_step(table, Link::wifi, 0) == CheckStep::retire);
    table.remove_wifi(old);
    check.retired();
    CHECK(check.next_step(table, Link::wifi, 1 * ms) == CheckStep::wait);
    CHECK(check.next_step(table, Link::usb, 1 * ms) == CheckStep::admit);
    check.end(table);
    CHECK(!table.usb()->hello_held);
  }

  {
    TEST("the old entry leaving by itself while asked: the newcomer is admitted");
    ClientTable table;
    IdentityCheck check;
    const int old = add_identified(table, 1, make_hello(office_id, true, 1));
    const int self = table.add_wifi(address(2), 0);
    check.begin(table, multiclient::wifi_seat(self), multiclient::wifi_seat(old), make_hello(office_id, true, 2), true, 0);
    check.next_step(table, Link::wifi, 0);
    check.asked(0);
    table.remove_wifi(old);
    CHECK(check.next_step(table, Link::wifi, 300 * ms) == CheckStep::admit);
  }

  {
    TEST("a different client in the old slot is not the old entry");
    ClientTable table;
    IdentityCheck check;
    const int old = add_identified(table, 1, make_hello(office_id, true, 1));
    const int self = table.add_wifi(address(2), 0);
    check.begin(table, multiclient::wifi_seat(self), multiclient::wifi_seat(old), make_hello(office_id, true, 2), true, 0);
    check.next_step(table, Link::wifi, 0);
    check.asked(0);
    table.remove_wifi(old);
    CHECK(add_identified(table, 4, make_hello(7, true, 1)) == old);
    CHECK(check.next_step(table, Link::wifi, 100 * ms) == CheckStep::admit);
  }

  {
    TEST("the newcomer leaving drops the check, on any link");
    ClientTable table;
    IdentityCheck check;
    const int old = add_identified(table, 1, make_hello(office_id, true, 1));
    int self = table.add_wifi(address(2), 0);
    check.begin(table, multiclient::wifi_seat(self), multiclient::wifi_seat(old), make_hello(office_id, true, 2), true, 0);
    table.remove_wifi(self);
    CHECK(check.next_step(table, Link::usb, 0) == CheckStep::drop);
    check.end(table);
    CHECK(!check.active());

    // Its slot taken by another connection in the meantime.
    self = table.add_wifi(address(2), 0);
    check.begin(table, multiclient::wifi_seat(self), multiclient::wifi_seat(old), make_hello(office_id, true, 2), true, 0);
    table.remove_wifi(self);
    CHECK(table.add_wifi(address(5), 0) == self);
    CHECK(check.next_step(table, Link::wifi, 0) == CheckStep::drop);
    check.end(table);
    CHECK(!table.wifi_at(self)->hello_held);

    // A USB newcomer whose link goes quiet and is cleared.
    start_usb(table);
    check.begin(table, multiclient::usb_seat(), multiclient::wifi_seat(old), make_hello(office_id, true, 2), true, 0);
    table.clear_usb_identity();
    CHECK(check.next_step(table, Link::wifi, 0) == CheckStep::drop);
    check.end(table);
  }

  {
    TEST("a pause in servicing the links while waiting asks again and restarts the wait");
    ClientTable table;
    IdentityCheck check;
    const int old = add_identified(table, 1, make_hello(office_id, true, 1));
    const int self = table.add_wifi(address(2), 0);
    check.begin(table, multiclient::wifi_seat(self), multiclient::wifi_seat(old), make_hello(office_id, true, 2), true, 0);
    check.next_step(table, Link::wifi, 0);
    check.asked(0);
    CHECK(check.next_step(table, Link::wifi, 200 * ms) == CheckStep::wait);
    // Nothing serviced for 5 s (a file transfer, say): ask again rather
    // than retire a controller whose answer could not be read.
    CHECK(check.next_step(table, Link::wifi, 5200 * ms) == CheckStep::ask);
    check.asked(5200 * ms);
    CHECK(poll_until(check, table, Link::wifi, 5300 * ms, 7100 * ms) == CheckStep::wait);
    uint32_t at = 0;
    CHECK(poll_until(check, table, Link::wifi, 7200 * ms, 7200 * ms, &at) == CheckStep::retire);
    // A pause of exactly the limit is not a pause.
    IdentityCheck second;
    ClientTable t2;
    const int o2 = add_identified(t2, 1, make_hello(office_id, true, 1));
    const int s2 = t2.add_wifi(address(2), 0);
    second.begin(t2, multiclient::wifi_seat(s2), multiclient::wifi_seat(o2), make_hello(office_id, true, 2), true, 0);
    second.next_step(t2, Link::wifi, 0);
    second.asked(0);
    CHECK(second.next_step(t2, Link::wifi, multiclient::presence_max_pause_us) == CheckStep::wait);
  }

  {
    TEST("the wait is measured correctly across a counter wrap");
    ClientTable table;
    IdentityCheck check;
    const int old = add_identified(table, 1, make_hello(office_id, true, 1));
    const int self = table.add_wifi(address(2), 0);
    const uint32_t start = 0xFFFFFFFFu - 1000 * ms;
    check.begin(table, multiclient::wifi_seat(self), multiclient::wifi_seat(old), make_hello(office_id, true, 2), true,
                start);
    check.next_step(table, Link::wifi, start);
    check.asked(start);
    CHECK(poll_until(check, table, Link::wifi, start + 100 * ms, start + 1900 * ms) == CheckStep::wait);
    CHECK(check.next_step(table, Link::wifi, start + 2000 * ms) == CheckStep::retire);
  }

  {
    TEST("a held newcomer is never old, and is again once the check ends");
    ClientTable table;
    IdentityCheck check;
    const int old = add_identified(table, 1, make_hello(office_id, true, 1));
    const int self = table.add_wifi(address(2), 0);
    const uint32_t late = multiclient::hello_window_us + 100 * ms;
    CHECK(multiclient::client_is_old(*table.wifi_at(self), late));
    check.begin(table, multiclient::wifi_seat(self), multiclient::wifi_seat(old), make_hello(office_id, true, 2), true,
                late - 50 * ms);
    CHECK(!multiclient::client_is_old(*table.wifi_at(self), late));
    CHECK(!table.has_old_client(late));
    check.end(table);
    CHECK(multiclient::client_is_old(*table.wifi_at(self), late));
    CHECK(table.has_old_client(late));
  }

  {
    TEST("end with nothing running changes nothing");
    ClientTable table;
    IdentityCheck check;
    check.end(table);
    CHECK(!check.active());
    CHECK(check.next_step(table, Link::wifi, 0) == CheckStep::wait);
    CHECK(!check.holds(multiclient::wifi_seat(0)));
  }

  {
    TEST("with a check running, a hello needing another check on a different id is told to retry");
    ClientTable table;
    IdentityCheck check;
    constexpr uint64_t shop_id = 0x0102030405060708ULL;
    const int old_a = add_identified(table, 1, make_hello(office_id, true, 1));
    const int new_a = table.add_wifi(address(2), 0);
    identify_usb(table, make_hello(shop_id, true, 7, "Shop PC"));
    const int new_b = table.add_wifi(address(3), 0);
    CHECK(check.begin(table, multiclient::wifi_seat(new_a), multiclient::wifi_seat(old_a),
                      make_hello(office_id, true, 2), true, 0));

    // shop_id from a different launch would need its own check.
    auto decision =
        multiclient::decide_hello(table, multiclient::wifi_seat(new_b), make_hello(shop_id, true, 8, "Shop PC"), check);
    CHECK(decision.action == HelloAction::busy);
    // shop_id from the same launch is a reconnect this WiFi link can finish
    // itself, so it needs no check and goes ahead.
    decision =
        multiclient::decide_hello(table, multiclient::wifi_seat(new_b), make_hello(shop_id, true, 7, "Shop PC"), check);
    CHECK(decision.action == HelloAction::replace);
    CHECK(decision.other.link == Link::usb);
    // An id nobody has is admitted as usual.
    CHECK(multiclient::decide_hello(table, multiclient::wifi_seat(new_b), make_hello(5, true, 1), check).action ==
          HelloAction::admit);
    // Without a check running the same hello would be asked.
    IdentityCheck idle;
    CHECK(multiclient::decide_hello(table, multiclient::wifi_seat(new_b), make_hello(shop_id, true, 8, "Shop PC"), idle)
              .action == HelloAction::ask);
  }

  {
    TEST("with a check running, a USB reconnect whose old entry is on WiFi is told to retry");
    ClientTable table;
    IdentityCheck check;
    constexpr uint64_t shop_id = 0x0102030405060708ULL;
    const int old_a = add_identified(table, 1, make_hello(office_id, true, 1));
    const int new_a = table.add_wifi(address(2), 0);
    add_identified(table, 3, make_hello(shop_id, true, 7, "Shop PC"));
    start_usb(table);
    CHECK(check.begin(table, multiclient::wifi_seat(new_a), multiclient::wifi_seat(old_a),
                      make_hello(office_id, true, 2), true, 0));
    CHECK(multiclient::decide_hello(table, multiclient::usb_seat(), make_hello(shop_id, true, 7, "Shop PC"), check)
              .action == HelloAction::busy);
    IdentityCheck idle;
    CHECK(multiclient::decide_hello(table, multiclient::usb_seat(), make_hello(shop_id, true, 7, "Shop PC"), idle)
              .action == HelloAction::replace);
  }

  {
    TEST("with a check running, any hello with the id being checked is told to retry");
    ClientTable table;
    IdentityCheck check;
    const int old_a = add_identified(table, 1, make_hello(office_id, true, 1));
    const int new_a = table.add_wifi(address(2), 0);
    const int other = table.add_wifi(address(3), 0);
    CHECK(check.begin(table, multiclient::wifi_seat(new_a), multiclient::wifi_seat(old_a),
                      make_hello(office_id, true, 2), true, 0));
    // The old entry's own launch: inline replacement would remove the entry
    // the check is asking, and two entries would end up with one id.
    CHECK(multiclient::decide_hello(table, multiclient::wifi_seat(other), make_hello(office_id, true, 1), check).action ==
          HelloAction::busy);
    // The held newcomer's launch, or another one.
    CHECK(multiclient::decide_hello(table, multiclient::wifi_seat(other), make_hello(office_id, true, 2), check).action ==
          HelloAction::busy);
    CHECK(multiclient::decide_hello(table, multiclient::wifi_seat(other), make_hello(office_id, false, 0), check).action ==
          HelloAction::busy);
    // Also from USB.
    start_usb(table);
    CHECK(multiclient::decide_hello(table, multiclient::usb_seat(), make_hello(office_id, true, 1), check).action ==
          HelloAction::busy);
    // Once the old entry has left and the newcomer is admitted, the same
    // hello is checked against the newcomer.
    table.remove_wifi(old_a);
    CHECK(check.next_step(table, Link::wifi, 10 * ms) == CheckStep::admit);
    const Hello admitted = check.hello();
    check.end(table);
    ControlToken unused;
    multiclient::admit_hello(*table.wifi_at(new_a), admitted, unused);
    const auto decision =
        multiclient::decide_hello(table, multiclient::wifi_seat(other), make_hello(office_id, true, 1), check);
    CHECK(decision.action == HelloAction::ask);
    CHECK(decision.other.index == new_a);
  }

  {
    TEST("two hellos with the same id: the second retries and is refused once the first is");
    ClientTable table;
    IdentityCheck check;
    const int old_a = add_identified(table, 1, make_hello(office_id, true, 1));
    const int first = table.add_wifi(address(2), 0);
    const int second = table.add_wifi(address(3), 0);
    CHECK(check.begin(table, multiclient::wifi_seat(first), multiclient::wifi_seat(old_a),
                      make_hello(office_id, true, 2), true, 0));
    CHECK(multiclient::decide_hello(table, multiclient::wifi_seat(second), make_hello(office_id, true, 3), check).action ==
          HelloAction::busy);
    CHECK(check.next_step(table, Link::wifi, 0) == CheckStep::ask);
    const uint32_t number = question_number(check);
    check.asked(0);
    CHECK(answer(check, multiclient::wifi_seat(old_a), number));
    CHECK(check.next_step(table, Link::wifi, 50 * ms) == CheckStep::refuse);
    check.end(table);
    // The retry starts its own check of the same, live, entry.
    const auto decision =
        multiclient::decide_hello(table, multiclient::wifi_seat(second), make_hello(office_id, true, 3), check);
    CHECK(decision.action == HelloAction::ask);
    CHECK(decision.other.index == old_a);
    CHECK(check.begin(table, multiclient::wifi_seat(second), decision.other, make_hello(office_id, true, 3), true,
                      1000 * ms));
    CHECK(check.next_step(table, Link::wifi, 1000 * ms) == CheckStep::ask);
    CHECK(question_number(check) != number);
  }

  {
    TEST("two checks on different ids run one after the other");
    ClientTable table;
    IdentityCheck check;
    constexpr uint64_t shop_id = 0x0102030405060708ULL;
    const int old_a = add_identified(table, 1, make_hello(office_id, true, 1));
    const int new_a = table.add_wifi(address(2), 0);
    identify_usb(table, make_hello(shop_id, true, 7, "Shop PC"));
    const int new_b = table.add_wifi(address(3), 0);
    CHECK(check.begin(table, multiclient::wifi_seat(new_a), multiclient::wifi_seat(old_a),
                      make_hello(office_id, true, 2), true, 0));
    CHECK(multiclient::decide_hello(table, multiclient::wifi_seat(new_b), make_hello(shop_id, true, 8, "Shop PC"), check)
              .action == HelloAction::busy);
    // The first check times out and is resolved.
    CHECK(check.next_step(table, Link::wifi, 0) == CheckStep::ask);
    check.asked(0);
    CHECK(poll_until(check, table, Link::wifi, 100 * ms, 2100 * ms) == CheckStep::retire);
    table.remove_wifi(old_a);
    check.retired();
    CHECK(check.next_step(table, Link::wifi, 2100 * ms) == CheckStep::admit);
    check.end(table);
    // The second hello, sent again, now gets its check.
    const auto decision =
        multiclient::decide_hello(table, multiclient::wifi_seat(new_b), make_hello(shop_id, true, 8, "Shop PC"), check);
    CHECK(decision.action == HelloAction::ask);
    CHECK(decision.other.link == Link::usb);
    CHECK(check.begin(table, multiclient::wifi_seat(new_b), decision.other, make_hello(shop_id, true, 8, "Shop PC"),
                      true, 2200 * ms));
    CHECK(check.next_step(table, Link::usb, 2200 * ms) == CheckStep::ask);
  }

  {
    TEST("a hello told to retry gets a fresh hello window, so it is not treated as old");
    ClientTable table;
    const int self = table.add_wifi(address(2), 0);
    CHECK(!multiclient::client_is_old(*table.wifi_at(self), 4900 * ms));
    multiclient::restart_hello_window(*table.wifi_at(self), 4900 * ms);
    CHECK(!multiclient::client_is_old(*table.wifi_at(self), 6000 * ms));
    CHECK(!table.has_old_client(9800 * ms));
    CHECK(multiclient::client_is_old(*table.wifi_at(self), 9900 * ms));

    start_usb(table, 0);
    multiclient::restart_hello_window(*table.usb(), 4000 * ms);
    CHECK(!multiclient::client_is_old(*table.usb(), 8000 * ms));
    CHECK(multiclient::client_is_old(*table.usb(), 9000 * ms));

    // A client whose window never started (USB before its first byte) is
    // left alone.
    Client quiet;
    multiclient::restart_hello_window(quiet, 100 * ms);
    CHECK(!quiet.hello_window_started);
  }

  {
    TEST("constants");
    CHECK(multiclient::presence_answer_wait_us == 2000000);
    CHECK(multiclient::presence_check_length == 4);
    CHECK(multiclient::hello_result_identity_connected == 3);
    CHECK(multiclient::hello_result_busy == 4);
    CHECK(&multiclient::shared_identity_check() == &multiclient::shared_identity_check());
  }

  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
