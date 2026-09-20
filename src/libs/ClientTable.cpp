#include "ClientTable.h"

namespace multiclient {

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

ClientTable& shared_client_table() {
  static ClientTable table;
  return table;
}

}  // namespace multiclient
