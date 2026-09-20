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

void record_heartbeat(Client& client, uint32_t now_ms) { client.last_heartbeat_ms = now_ms; }

bool client_is_old(const Client& client, uint32_t now_ms) {
  if (client.identified) return false;
  if (!client.hello_window_started) return false;
  return now_ms - client.hello_window_start_ms >= hello_window_ms;
}

bool usb_session_expired(bool hello_window_started, uint32_t now_ms, uint32_t last_activity_ms) {
  if (!hello_window_started) return false;
  const int32_t elapsed_ms = static_cast<int32_t>(now_ms - last_activity_ms);
  return elapsed_ms >= static_cast<int32_t>(usb_idle_timeout_ms);
}

int ClientTable::find_wifi(const Address& address) const {
  for (std::size_t i = 0; i < max_wifi_clients; ++i) {
    if (wifi_[i].in_use && same_address(wifi_[i].client.address, address)) return static_cast<int>(i);
  }
  return -1;
}

int ClientTable::add_wifi(const Address& address, uint32_t now_ms) {
  const int existing = find_wifi(address);
  if (existing >= 0) return existing;

  for (std::size_t i = 0; i < max_wifi_clients; ++i) {
    if (wifi_[i].in_use) continue;
    wifi_[i].in_use = true;
    wifi_[i].client = Client{};
    wifi_[i].client.link = Link::wifi;
    wifi_[i].client.address = address;
    wifi_[i].client.last_user_ms = now_ms;
    wifi_[i].client.last_heartbeat_ms = now_ms;
    // The firmware only learns a WiFi client exists once it has sent
    // something, so "now" is the closest approximation available to when
    // the TCP connection was actually accepted -- the hello window starts
    // here.
    wifi_[i].client.hello_window_started = true;
    wifi_[i].client.hello_window_start_ms = now_ms;
    return static_cast<int>(i);
  }
  return -1;
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

void ClientTable::set_usb_present(bool present, uint32_t now_ms) {
  if (!present) {
    usb_ = Slot{};
    return;
  }
  if (usb_.in_use) return;
  usb_.in_use = true;
  usb_.client = Client{};
  usb_.client.link = Link::usb;
  usb_.client.last_user_ms = now_ms;
  usb_.client.last_heartbeat_ms = now_ms;
}

Client* ClientTable::usb() { return usb_.in_use ? &usb_.client : nullptr; }

const Client* ClientTable::usb() const { return usb_.in_use ? &usb_.client : nullptr; }

void ClientTable::start_usb_hello_window(uint32_t now_ms) {
  if (!usb_.in_use || usb_.client.hello_window_started) return;
  usb_.client.hello_window_started = true;
  usb_.client.hello_window_start_ms = now_ms;
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

bool ClientTable::has_old_client(uint32_t now_ms, int except_wifi_index, bool exclude_usb) const {
  for (std::size_t i = 0; i < max_wifi_clients; ++i) {
    if (static_cast<int>(i) == except_wifi_index || !wifi_[i].in_use) continue;
    if (client_is_old(wifi_[i].client, now_ms)) return true;
  }
  if (exclude_usb) return false;
  return usb_.in_use && client_is_old(usb_.client, now_ms);
}

bool ClientTable::any_identified_present() const {
  for (std::size_t i = 0; i < max_wifi_clients; ++i) {
    if (wifi_[i].in_use && wifi_[i].client.identified) return true;
  }
  return usb_.in_use && usb_.client.identified;
}

bool ClientTable::is_earliest_present(int wifi_index) const {
  const Client* candidate = wifi_at(wifi_index);
  if (candidate == nullptr) return false;

  for (std::size_t i = 0; i < max_wifi_clients; ++i) {
    if (static_cast<int>(i) == wifi_index || !wifi_[i].in_use) continue;
    if (ms_before(wifi_[i].client.hello_window_start_ms, candidate->hello_window_start_ms)) return false;
  }
  if (usb_.in_use && usb_.client.hello_window_started &&
      ms_before(usb_.client.hello_window_start_ms, candidate->hello_window_start_ms)) {
    return false;
  }
  return true;
}

ClientTable& shared_client_table() {
  static ClientTable table;
  return table;
}

}  // namespace multiclient
