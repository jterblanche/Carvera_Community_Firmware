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

// How long a USB entry can go with no frame at all before its session is
// considered over: its identity and hello-window progress are reset, the
// same as a protocol switch does, so it stops counting as present until
// something arrives on it again. This is unrelated to hello_window_ms
// above (5 s, how long an unidentified client has before it is old) --
// it matches the WiFi module's own default dead-client timeout instead
// (10 s, kept as one rule across links).
constexpr uint32_t usb_idle_timeout_ms = 10000;

// True if `a` happened before `b`, correct across a millisecond-counter
// wrap: the same signed-subtraction idiom every timeout check in this
// codebase already relies on, applied to a direct comparison between two
// timestamps instead of a "how long ago" check.
constexpr bool ms_before(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) < 0;
}

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

// Records that `client` sent a heartbeat at `now_ms`. Accepted from any
// connected client, identified or not -- the WiFi module's own idle timeout
// already treats traffic in either direction as keeping a link alive (see
// the protocol contract's heartbeat section for why the heartbeat message
// is kept anyway); this firmware only needs to remember when it last heard
// one, not act on it itself.
void record_heartbeat(Client& client, uint32_t now_ms);

// True once `client`'s hello window has run out without it identifying.
// Always false once identified, and false while the window has not started
// (an idle USB link) or has not elapsed yet. now_ms wraps the same way every
// other millisecond timestamp in this firmware does; the subtraction is
// correct across a wrap.
bool client_is_old(const Client& client, uint32_t now_ms);

// True once a USB entry's hello window has started and at least
// usb_idle_timeout_ms has passed since the last byte received on it
// (`last_activity_ms`, tracked by the caller -- SerialConsole, not this
// table). When this becomes true, the caller is expected to call
// ClientTable::clear_usb_identity() to end that session; false again
// immediately afterwards, since hello_window_started is then false.
//
// The caller samples `now_ms` and then reads `last_activity_ms` as two
// separate steps, and `last_activity_ms` is written from an interrupt that
// can fire in between -- so `last_activity_ms` can end up later than the
// `now_ms` already sampled, meaning a byte arrived after the check started.
// The subtraction is done as a signed difference specifically so that
// case (a timestamp that turns out to be in the future) yields a small
// negative number, safely less than the timeout, rather than the huge
// value plain unsigned subtraction would wrap around to -- which would
// otherwise read as "expired" and end an actively-talking session.
bool usb_session_expired(bool hello_window_started, uint32_t now_ms, uint32_t last_activity_ms);

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

  // True if some currently-present client (WiFi or USB) is identified.
  // Used by the old-client rule: while nobody present has identified, an
  // old client is not disconnected just because another present client
  // hasn't decided yet (is simply still within its own window) -- see
  // is_earliest_present() below for how that case is actually settled.
  // Once anyone present has identified, this is true, because a hello can
  // never succeed while an old client is already known to be in the mix
  // (see has_old_client()) -- so an identified peer's mere presence
  // already proves no old client needs protecting from it.
  bool any_identified_present() const;

  // True if WiFi index `wifi_index` is the earliest-admitted client
  // currently present (WiFi or USB), by hello_window_start_ms. Ties (an
  // identical timestamp) are treated as "earliest" on both sides, so nobody
  // is evicted in that vanishingly unlikely case. Used together with
  // any_identified_present() to decide which old WiFi client to spare:
  // while nobody present has identified, the earliest-admitted one present
  // is never disconnected by the old-client rule, whether or not it has
  // itself been decided old yet -- matching today's single-client world,
  // where whoever connected first holds the link regardless of anyone
  // else's timing.
  bool is_earliest_present(int wifi_index) const;

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
