#include "ClientTable.h"

#include <cstring>

namespace multiclient {

void set_identity(Client& client, uint64_t id, const char* name, uint8_t name_len) {
  if (name_len > max_name_length) name_len = max_name_length;
  client.id = id;
  client.name_len = name_len;
  if (name_len != 0) std::memcpy(client.name, name, name_len);
  client.name[name_len] = '\0';
  client.identified = true;
}

void record_heartbeat(Client& client, uint32_t now_us) { client.last_heartbeat_us = now_us; }

bool client_is_old(const Client& client, uint32_t now_us) {
  if (client.identified) return false;
  if (!client.hello_window_started) return false;
  const int32_t elapsed_us = static_cast<int32_t>(now_us - client.hello_window_start_us);
  return elapsed_us >= static_cast<int32_t>(hello_window_us);
}

bool usb_session_expired(bool hello_window_started, uint32_t now_us, uint32_t last_activity_us) {
  if (!hello_window_started) return false;
  const int32_t elapsed_us = static_cast<int32_t>(now_us - last_activity_us);
  return elapsed_us >= static_cast<int32_t>(usb_idle_timeout_us);
}

int ClientTable::find_wifi(const Address& address) const {
  for (std::size_t i = 0; i < max_wifi_clients; ++i) {
    if (wifi_[i].in_use && same_address(wifi_[i].client.address, address)) return static_cast<int>(i);
  }
  return -1;
}

int ClientTable::add_wifi(const Address& address, uint32_t now_us) {
  const int existing = find_wifi(address);
  if (existing >= 0) return existing;

  for (std::size_t i = 0; i < max_wifi_clients; ++i) {
    if (wifi_[i].in_use) continue;
    wifi_[i].in_use = true;
    wifi_[i].client = Client{};
    wifi_[i].client.link = Link::wifi;
    wifi_[i].client.address = address;
    wifi_[i].client.last_user_us = now_us;
    wifi_[i].client.last_heartbeat_us = now_us;
    // The firmware only learns a WiFi client exists once it has sent
    // something, so "now" is the closest approximation available to when
    // the TCP connection was actually accepted -- the hello window starts
    // here.
    wifi_[i].client.hello_window_started = true;
    wifi_[i].client.hello_window_start_us = now_us;
    return static_cast<int>(i);
  }
  return -1;
}

bool send_error_means_client_gone(uint8_t errcode) {
  return errcode == 0x14   // connection by link_no not present
      || errcode == 0x15   // connection by link_no closed
      || errcode == 0x1A;  // no such client
}

void note_send_result(Client& client, bool sent_everything, uint8_t errcode) {
  if (sent_everything) {
    client.consecutive_send_failures = 0;
    return;
  }
  if (send_error_means_client_gone(errcode)) {
    client.send_failed = true;
    return;
  }
  if (client.consecutive_send_failures < 0xFF) ++client.consecutive_send_failures;
  if (client.consecutive_send_failures >= max_consecutive_send_failures) client.send_failed = true;
}

bool ClientTable::remove_wifi(int index) {
  if (index < 0 || static_cast<std::size_t>(index) >= max_wifi_clients || !wifi_[index].in_use) return false;
  wifi_[index] = Slot{};
  return true;
}

Client* ClientTable::wifi_at(int index) {
  if (index < 0 || static_cast<std::size_t>(index) >= max_wifi_clients || !wifi_[index].in_use) return nullptr;
  return &wifi_[index].client;
}

const Client* ClientTable::wifi_at(int index) const {
  if (index < 0 || static_cast<std::size_t>(index) >= max_wifi_clients || !wifi_[index].in_use) return nullptr;
  return &wifi_[index].client;
}

std::size_t ClientTable::wifi_count() const {
  std::size_t count = 0;
  for (const Slot& slot : wifi_) {
    if (slot.in_use) ++count;
  }
  return count;
}

void ClientTable::clear_wifi() {
  for (Slot& slot : wifi_) slot = Slot{};
}

void ClientTable::set_usb_present(bool present, uint32_t now_us) {
  if (!present) {
    usb_ = Slot{};
    return;
  }
  if (usb_.in_use) return;
  usb_.in_use = true;
  usb_.client = Client{};
  usb_.client.link = Link::usb;
  usb_.client.last_user_us = now_us;
  usb_.client.last_heartbeat_us = now_us;
}

Client* ClientTable::usb() { return usb_.in_use ? &usb_.client : nullptr; }

const Client* ClientTable::usb() const { return usb_.in_use ? &usb_.client : nullptr; }

void ClientTable::start_usb_hello_window(uint32_t now_us) {
  if (!usb_.in_use || usb_.client.hello_window_started) return;
  usb_.client.hello_window_started = true;
  usb_.client.hello_window_start_us = now_us;
}

void ClientTable::clear_usb_identity() {
  if (!usb_.in_use) return;
  usb_.client = Client{};
  usb_.client.link = Link::usb;
}

std::size_t ClientTable::present_count() const {
  std::size_t count = wifi_count();
  if (usb_.in_use && usb_.client.hello_window_started) ++count;
  return count;
}

int ClientTable::find_wifi_by_id(uint64_t id, int except_index) const {
  for (std::size_t i = 0; i < max_wifi_clients; ++i) {
    if (static_cast<int>(i) == except_index || !wifi_[i].in_use) continue;
    if (wifi_[i].client.identified && wifi_[i].client.id == id) return static_cast<int>(i);
  }
  return -1;
}

bool ClientTable::usb_has_id(uint64_t id) const {
  return usb_.in_use && usb_.client.identified && usb_.client.id == id;
}

bool ClientTable::has_old_client(uint32_t now_us, int except_wifi_index, bool exclude_usb) const {
  for (std::size_t i = 0; i < max_wifi_clients; ++i) {
    if (static_cast<int>(i) == except_wifi_index || !wifi_[i].in_use) continue;
    if (client_is_old(wifi_[i].client, now_us)) return true;
  }
  if (exclude_usb) return false;
  return usb_.in_use && client_is_old(usb_.client, now_us);
}

bool ClientTable::any_identified_present() const {
  for (std::size_t i = 0; i < max_wifi_clients; ++i) {
    if (wifi_[i].in_use && wifi_[i].client.identified) return true;
  }
  return usb_.in_use && usb_.client.identified;
}

namespace {

// The USB entry has no WiFi slot index of its own; for the tiebreak below it
// sorts before every WiFi index (0, 1, 2, ...), so a tie between USB and a
// WiFi client resolves in USB's favour.
constexpr int usb_slot_index = -1;

// True if the client admitted at (ts_a, slot_a) is strictly earlier than the
// one at (ts_b, slot_b). Equal timestamps are broken by slot index, so this
// gives a strict order even when both clients were admitted in the same
// microsecond.
bool admitted_before(uint32_t ts_a, int slot_a, uint32_t ts_b, int slot_b) {
  if (ts_a != ts_b) return before(ts_a, ts_b);
  return slot_a < slot_b;
}

}  // namespace

bool ClientTable::is_earliest_present(int wifi_index) const {
  const Client* candidate = wifi_at(wifi_index);
  if (candidate == nullptr) return false;

  for (std::size_t i = 0; i < max_wifi_clients; ++i) {
    if (static_cast<int>(i) == wifi_index || !wifi_[i].in_use) continue;
    if (admitted_before(wifi_[i].client.hello_window_start_us, static_cast<int>(i),
                         candidate->hello_window_start_us, wifi_index)) {
      return false;
    }
  }
  if (usb_.in_use && usb_.client.hello_window_started &&
      admitted_before(usb_.client.hello_window_start_us, usb_slot_index,
                       candidate->hello_window_start_us, wifi_index)) {
    return false;
  }
  return true;
}

ClientTable& shared_client_table() {
  static ClientTable table;
  return table;
}

bool is_transfer_owner(const ClientTable& table, int client_index, const Address& sender) {
  const Client* owner = table.wifi_at(client_index);
  return owner != nullptr && same_address(owner->address, sender);
}

}  // namespace multiclient
