#pragma once

#include <cstddef>
#include <cstdint>

#include "ClientTable.h"
#include "ControlToken.h"
#include "Hello.h"

// What the machine does when a hello carries the id of a controller that is
// already connected on another link. Plain C++, no Kernel or mbed
// dependency, so it builds and runs on the host (see tests/TEST_IdentityCheck).
//
// - Same launch part, or neither sent one: the same running controller has
//   come back on a new link, so its old link is dead. The old entry is
//   replaced at once.
// - Different launch part: the connected controller is asked whether it is
//   still there (PTYPE_PRESENCE_CHECK) and the new hello is held unanswered
//   for up to presence_answer_wait_us. If it answers, the new hello is
//   refused with hello_result_identity_connected and the connected
//   controller keeps its connection and control. If not, its entry is
//   removed and the new controller is admitted.
//
// Either way the controller admitted under that id starts without control.
// One check runs at a time. A hello that would need another one, or that
// carries the id being checked, is answered hello_result_busy and sent
// again by its controller shortly after.
// The links (WifiProvider, SerialConsole) do the sending and removing; this
// file only decides.
namespace multiclient {

// Where a client sits in the shared table: a WiFi slot, or the USB entry.
struct Seat {
  Link link = Link::wifi;
  int8_t index = -1;  // the WiFi slot; -1 for USB
};

inline Seat wifi_seat(int index) { return Seat{Link::wifi, static_cast<int8_t>(index)}; }
inline Seat usb_seat() { return Seat{Link::usb, -1}; }
inline bool same_seat(const Seat& a, const Seat& b) { return a.link == b.link && a.index == b.index; }

// The client at `seat`, or nullptr if that slot is empty.
Client* client_at(ClientTable& table, const Seat& seat);
const Client* client_at(const ClientTable& table, const Seat& seat);

// True if `client` and `hello` come from the same launch of a controller:
// both carry the same launch part, or neither carries one.
bool same_launch(const Client& client, const Hello& hello);

// How long a connected controller has to answer a presence check before it
// is treated as gone. A live controller answers in well under 100 ms.
constexpr uint32_t presence_answer_wait_us = 2000000;

// If the links were not serviced for longer than this while waiting for an
// answer (a file transfer or a long command holds up the loop that reads
// them), the answer may have arrived unread, so the question is asked
// again and the wait starts over.
constexpr uint32_t presence_max_pause_us = 250000;

// Length of a presence check and of its reply: number(4, big-endian).
constexpr std::size_t presence_check_length = 4;

enum class HelloAction : uint8_t {
  admit,    // nobody else has this id
  replace,  // another entry has it, from the same launch: replace it now
  ask,      // another entry has it, from a different launch: ask it first
  busy,     // a check is running that this hello must wait for: try again shortly
};

struct HelloDecision {
  HelloAction action = HelloAction::admit;
  Seat other;  // the entry holding the id, unless action is admit
};

// Decides what to do with a first hello from `self`, by looking for another
// identified entry with the same id: a WiFi slot other than `self`, or,
// for a WiFi sender, the USB entry.
HelloDecision decide_hello(const ClientTable& table, const Seat& self, const Hello& hello);

// Records `hello` on `self` and marks it identified. If control is held
// under the same id, by an entry this hello replaces or one that has just
// left, it is freed, so the controller admitted starts without it. Returns
// true when control was freed: the caller publishes a control-changed event
// naming nobody.
bool admit_hello(Client& self, const Hello& hello, ControlToken& control);

// What a link should do next for the check in progress.
enum class CheckStep : uint8_t {
  wait,    // nothing for this link to do now
  ask,     // send old() a presence check carrying build_question(), then call asked()
  retire,  // remove old()'s entry, then call retired()
  refuse,  // answer newcomer() with hello_result_identity_connected, then call end()
  admit,   // copy hello() and call end(), then admit newcomer() with it (reconnect)
  drop,    // newcomer() has gone: call end() and send nothing
};

// The one check that can be in progress at a time: a held hello, the entry
// it is waiting on, and the question asked.
class IdentityCheck {
 public:
  bool active() const { return phase_ != Phase::idle; }

  // Starts a check of `old` for `hello` from `newcomer`, which is held:
  // `ask_first` asks `old` whether it is still there; otherwise `old` is
  // removed straight away (a reconnect whose old entry is on the other
  // link). Returns false, changing nothing, if a check is already running.
  bool begin(ClientTable& table, const Seat& newcomer, const Seat& old, const Hello& hello, bool ask_first,
             uint32_t now_us);

  // True if `seat` is the newcomer whose hello is being held.
  bool holds(const Seat& seat) const { return active() && same_seat(seat, newcomer_); }

  // The next step for `link`. Each link calls this from its idle loop, and
  // straight after begin(), and carries out each step it is given until it
  // gets wait. Steps for the newcomer's entry go to the newcomer's link and
  // steps for the old entry to the old entry's link, so each link only
  // touches its own entries.
  CheckStep next_step(const ClientTable& table, Link link, uint32_t now_us);

  // Writes the presence check's payload, the current number, to `out`
  // (presence_check_length bytes). Returns presence_check_length.
  std::size_t build_question(uint8_t* out) const;
  void asked(uint32_t now_us);
  void retired();

  // A presence reply from `from`. Counts only from the entry asked, with
  // the number last asked, while waiting for it. Returns true if it counted.
  bool note_answer(const Seat& from, const uint8_t* data, std::size_t length);

  // Ends the check, releasing the newcomer's hold if it is still there.
  void end(ClientTable& table);

  const Hello& hello() const { return hello_; }
  const Seat& newcomer() const { return newcomer_; }
  const Seat& old() const { return old_; }

 private:
  enum class Phase : uint8_t { idle, ask, waiting, retire, admit, refuse };

  bool newcomer_present(const ClientTable& table) const;
  bool old_present(const ClientTable& table) const;

  Phase phase_ = Phase::idle;
  Seat newcomer_;
  Seat old_;
  Address newcomer_address_;
  uint32_t number_ = 0;
  uint32_t asked_us_ = 0;
  uint32_t last_step_us_ = 0;
  Hello hello_;
};

// decide_hello() above, given the check that may be running. With a check
// running, the answer is busy for a hello that carries the id being checked
// (whatever its launch part, since the entry being asked might otherwise be
// replaced under the check), and for one that would need a check itself: a
// different launch, or a USB hello whose old entry is on WiFi, which only
// the WiFi link can remove. Any other hello is decided as usual.
HelloDecision decide_hello(const ClientTable& table, const Seat& self, const Hello& hello, const IdentityCheck& check);

// One check, shared by the WiFi and USB links.
IdentityCheck& shared_identity_check();

}  // namespace multiclient
