#pragma once

#include <cstddef>
#include <cstdint>

namespace multiclient {

// Which physical link a client is on.
enum class Link : uint8_t { wifi, usb };

// A TCP client's address. Unused (all zero) for the USB entry, which has no
// address to key on -- there is only ever one USB link.
struct Address {
  uint8_t ip[4] = {0, 0, 0, 0};
  uint16_t port = 0;
};

inline bool same_address(const Address& a, const Address& b) {
  return a.port == b.port && a.ip[0] == b.ip[0] && a.ip[1] == b.ip[1] && a.ip[2] == b.ip[2] && a.ip[3] == b.ip[3];
}

// One connected client. `identified` and the two timestamps are written by
// whoever runs the identify handshake and the control gate; this module only
// stores them.
struct Client {
  Link link = Link::wifi;
  Address address;
  bool identified = false;
  uint32_t last_user_ms = 0;
  uint32_t last_heartbeat_ms = 0;
};

// Engineering limit: 3 WiFi clients plus the one USB link. See version.txt
// and WifiProvider.cpp for why the WiFi module's own client limit is kept
// above this figure.
constexpr std::size_t max_wifi_clients = 3;

// The connected-client table: up to `max_wifi_clients` WiFi sockets plus one
// USB entry. Plain C++, no Kernel or mbed dependency, so it builds and runs
// on the host (see tests/TEST_ClientTable). This module only tracks who is
// connected; it never decides who to disconnect -- callers enforce the cap
// themselves by refusing whatever `add_wifi` turns away.
class ClientTable {
 public:
  ClientTable() = default;

  // Adds a WiFi client at `address`. Returns its index (0..max_wifi_clients-1)
  // on success. Returns -1 when the cap is already reached; the table never
  // evicts an existing client to make room, so the caller must refuse the
  // new connection itself. Adding an address already present is a no-op that
  // returns its existing index (a reconnect before the old entry was reaped
  // does not consume a second slot).
  int add_wifi(const Address& address, uint32_t now_ms);

  // Removes the WiFi client at `index`. Returns false if `index` is out of
  // range or already empty.
  bool remove_wifi(int index);

  // Returns the index of the WiFi client at `address`, or -1 if none.
  int find_wifi(const Address& address) const;

  // Returns the client at `index`, or nullptr if `index` is out of range or
  // the slot is empty. The returned pointer may be used to update
  // `identified`, `last_user_ms` and `last_heartbeat_ms`; it is invalidated
  // by any add_wifi/remove_wifi/clear_wifi call.
  Client* wifi_at(int index);
  const Client* wifi_at(int index) const;

  std::size_t wifi_count() const;
  static constexpr std::size_t wifi_capacity() { return max_wifi_clients; }

  // Empties every WiFi slot (for example after a protocol switch, which
  // invalidates every WiFi client's parser state).
  void clear_wifi();

  // The single USB entry. `set_usb_present(true, ...)` creates it if absent;
  // `set_usb_present(false, ...)` removes it. USB has no address, so unlike
  // WiFi there is nothing to key a reconnect on.
  void set_usb_present(bool present, uint32_t now_ms);
  Client* usb();
  const Client* usb() const;

 private:
  struct Slot {
    bool in_use = false;
    Client client;
  };

  Slot wifi_[max_wifi_clients];
  Slot usb_;
};

// One table, shared by the WiFi and USB links, so the 3 WiFi + 1 USB cap is
// enforced across both.
ClientTable& shared_client_table();

}  // namespace multiclient
