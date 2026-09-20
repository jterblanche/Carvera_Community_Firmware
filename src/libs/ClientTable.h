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

// Longest name accepted from a hello, matching the wire format's one-byte
// length prefix.
constexpr std::size_t max_name_length = 31;

// How long a client has, from the moment its hello window starts, to send
// hello before it is treated as an old (pre-identify) client.
constexpr uint32_t hello_window_ms = 5000;

// One connected client. `identified`, `id`, `name`, `name_len` and the two
// timestamps are written by whoever runs the identify handshake and the
// control gate; this module only stores them.
struct Client {
  Link link = Link::wifi;
  Address address;
  bool identified = false;
  uint64_t id = 0;
  char name[max_name_length + 1] = {0};  // NUL-terminated
  uint8_t name_len = 0;                  // length excluding the NUL, 0..max_name_length
  uint32_t last_user_ms = 0;
  uint32_t last_heartbeat_ms = 0;

  // When this client's hello window started. For a WiFi client this is set
  // as soon as the table admits it (add_wifi) -- the firmware only learns a
  // WiFi client exists once it has sent something, which is close enough to
  // the TCP connection being accepted in practice. For USB there is no
  // connect event to key on at all (the firmware cannot detect a bare
  // cable), so the slot exists from boot with the window not yet started; a
  // caller starts it explicitly on the first byte actually received
  // (ClientTable::start_usb_hello_window) -- a USB controller only counts as
  // present once it is actually talking.
  bool hello_window_started = false;
  uint32_t hello_window_start_ms = 0;
};

// Records `id`/`name` on `client` and marks it identified. Does not touch
// the hello-window fields. `name_len` beyond max_name_length is clamped (the
// caller should already have refused a longer name; this is a second line
// of defence).
void set_identity(Client& client, uint64_t id, const char* name, uint8_t name_len);

// True once `client`'s hello window has run out without it identifying.
// Always false once identified, and false while the window has not started
// (an idle USB link) or has not elapsed yet. now_ms wraps the same way every
// other millisecond timestamp in this firmware does; the subtraction is
// correct across a wrap.
bool client_is_old(const Client& client, uint32_t now_ms);

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

  // Marks the USB entry as now talking (its hello window starts), if it is
  // present and hasn't already -- see Client::hello_window_started. A no-op
  // if the USB slot is absent or already started.
  void start_usb_hello_window(uint32_t now_ms);

  // Resets the USB entry's identity and hello-window progress without
  // removing the slot itself (there is nothing to reconnect for USB). Used
  // after a protocol switch, which invalidates every client's identified
  // state.
  void clear_usb_identity();

  // Number of clients considered actively present right now: every WiFi
  // slot in use, plus the USB slot only once its hello window has started.
  // Used by the old-client rule below to decide "alone" vs "not alone".
  std::size_t present_count() const;

  // Index of an already-identified WiFi client holding `id`, other than
  // `except_index`, or -1. Used to detect a reconnect under an id already in
  // the table.
  int find_wifi_by_id(uint64_t id, int except_index = -1) const;

  // True if the USB entry is identified and holds `id`.
  bool usb_has_id(uint64_t id) const;

  // True if some client other than wifi index `except_wifi_index` (pass -1
  // to exclude none) and, if `exclude_usb` is true, other than the USB
  // entry, is currently old (see client_is_old). Used to refuse a new hello
  // while an old controller is already known to be in the mix -- excluding
  // whichever client is the one currently saying hello, so a client can
  // never see itself as the reason its own hello is refused -- and to
  // decide whether the discovery beacon should report one present (no
  // exclusions there).
  bool has_old_client(uint32_t now_ms, int except_wifi_index = -1, bool exclude_usb = false) const;

 private:
  struct Slot {
    bool in_use = false;
    Client client;
  };

  Slot wifi_[max_wifi_clients];
  Slot usb_;
};

// One table, shared by the WiFi and USB links, so both are tracked in one
// place. The two are independent: the 3-client WiFi cap is enforced only
// among the WiFi slots (`add_wifi` never looks at `usb_`), and the single
// USB entry has no cap of its own to enforce -- there is only one USB link.
ClientTable& shared_client_table();

}  // namespace multiclient
