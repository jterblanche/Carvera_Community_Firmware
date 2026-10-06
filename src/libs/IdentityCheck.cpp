#include "IdentityCheck.h"

namespace multiclient {

Client* client_at(ClientTable& table, const Seat& seat) {
  return seat.link == Link::usb ? table.usb() : table.wifi_at(seat.index);
}

const Client* client_at(const ClientTable& table, const Seat& seat) {
  return seat.link == Link::usb ? table.usb() : table.wifi_at(seat.index);
}

bool same_launch(const Client& client, const Hello& hello) {
  if (client.has_launch != hello.has_launch) return false;
  return !hello.has_launch || client.launch == hello.launch;
}

HelloDecision decide_hello(const ClientTable& table, const Seat& self, const Hello& hello) {
  HelloDecision decision;
  const int wifi = table.find_wifi_by_id(hello.id, self.link == Link::wifi ? self.index : -1);
  if (wifi >= 0) {
    decision.other = wifi_seat(wifi);
  } else if (self.link == Link::wifi && table.usb_has_id(hello.id)) {
    decision.other = usb_seat();
  } else {
    return decision;
  }
  decision.action = same_launch(*client_at(table, decision.other), hello) ? HelloAction::replace : HelloAction::ask;
  return decision;
}

bool admit_hello(Client& self, const Hello& hello, ControlToken& control) {
  set_identity(self, hello.id, hello.name, hello.name_len);
  self.features = hello.features;
  self.has_launch = hello.has_launch;
  self.launch = hello.launch;
  return control.release_if_holder(hello.id);
}

bool IdentityCheck::begin(ClientTable& table, const Seat& newcomer, const Seat& old, const Hello& hello,
                          bool ask_first, uint32_t now_us) {
  if (active()) return false;
  Client* client = client_at(table, newcomer);
  if (client == nullptr) return false;
  client->hello_held = true;
  newcomer_ = newcomer;
  newcomer_address_ = client->address;
  old_ = old;
  hello_ = hello;
  last_step_us_ = now_us;
  phase_ = ask_first ? Phase::ask : Phase::retire;
  return true;
}

bool IdentityCheck::newcomer_present(const ClientTable& table) const {
  const Client* client = client_at(table, newcomer_);
  return client != nullptr && client->hello_held && !client->identified &&
         same_address(client->address, newcomer_address_);
}

bool IdentityCheck::old_present(const ClientTable& table) const {
  const Client* client = client_at(table, old_);
  return client != nullptr && client->identified && client->id == hello_.id;
}

CheckStep IdentityCheck::next_step(const ClientTable& table, Link link, uint32_t now_us) {
  if (!active()) return CheckStep::wait;
  if (!newcomer_present(table)) return CheckStep::drop;

  // The old entry left by itself, or was just removed: nothing to wait for.
  if (phase_ != Phase::refuse && !old_present(table)) phase_ = Phase::admit;

  if (phase_ == Phase::waiting && static_cast<uint32_t>(now_us - last_step_us_) > presence_max_pause_us) {
    phase_ = Phase::ask;
  }
  last_step_us_ = now_us;

  if (phase_ == Phase::waiting && static_cast<uint32_t>(now_us - asked_us_) >= presence_answer_wait_us) {
    phase_ = Phase::retire;
  }

  switch (phase_) {
    case Phase::ask: return link == old_.link ? CheckStep::ask : CheckStep::wait;
    case Phase::retire: return link == old_.link ? CheckStep::retire : CheckStep::wait;
    case Phase::refuse: return link == newcomer_.link ? CheckStep::refuse : CheckStep::wait;
    case Phase::admit: return link == newcomer_.link ? CheckStep::admit : CheckStep::wait;
    default: return CheckStep::wait;
  }
}

std::size_t IdentityCheck::build_question(uint8_t* out) const {
  for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(number_ >> (8 * (3 - i)));
  return presence_check_length;
}

void IdentityCheck::asked(uint32_t now_us) {
  if (phase_ != Phase::ask) return;
  asked_us_ = now_us;
  phase_ = Phase::waiting;
}

void IdentityCheck::retired() {
  if (phase_ == Phase::retire) phase_ = Phase::admit;
}

bool IdentityCheck::note_answer(const Seat& from, const uint8_t* data, std::size_t length) {
  if (phase_ != Phase::waiting || !same_seat(from, old_)) return false;
  if (data == nullptr || length < presence_check_length) return false;
  uint32_t number = 0;
  for (int i = 0; i < 4; ++i) number = (number << 8) | data[i];
  if (number != number_) return false;
  phase_ = Phase::refuse;
  return true;
}

void IdentityCheck::end(ClientTable& table) {
  if (!active()) return;
  if (newcomer_present(table)) client_at(table, newcomer_)->hello_held = false;
  phase_ = Phase::idle;
  ++number_;  // an answer to this check never counts for the next one
}

IdentityCheck& shared_identity_check() {
  static IdentityCheck check;
  return check;
}

}  // namespace multiclient
