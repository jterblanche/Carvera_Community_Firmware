#pragma once

#include <cstddef>
#include <cstdint>

#include "ClientTable.h"

// The control token: in single-user mode, exactly one identified client "has
// control" at a time -- the one whose last user-caused message arrived most
// recently (docs/research/connection-follows-me.md, sections 3 and 4.1).
// Automatic traffic never moves it. A user-caused message from a client that
// is not already the holder either seizes it silently, or is refused with a
// reason when interactive motion is in progress. Plain C++, no Kernel or
// mbed dependency, so it builds and runs on the host (see
// tests/TEST_ControlToken) the same way ClientTable and Publish do.
//
// Multi-user mode (control moves only when the holder releases it, a later
// stage) does not exist yet -- every hello ack this firmware sends already
// reports hello_mode_single_user (see Hello.h), and this file assumes that
// mode unconditionally.
namespace multiclient {

// Whether one inbound message is automatic (never moves the token) or
// user-caused (does). See classify_command_line() and
// classify_file_transfer_start() below for how a message is actually sorted
// into one of these.
enum class Traffic : uint8_t { automatic, user_caused };

// Classifies one ordinary command line already stripped of its wire framing
// -- the same bytes handed to THEKERNEL->dispatch_console_line(). Matches
// only the specific read-only, connect-time queries the design names as
// automatic (docs/research/connection-follows-me.md, section 4.1 and
// section 9.3): `model`, `version`, `ftype`, `time` (with or without an
// argument -- setting the clock is part of the same connect-time
// handshake as reading it, so both forms are automatic), and `get wcs`.
// Every other command word -- every G-code line, MDI, jog, probe, homing,
// spindle, override, play/abort/suspend/resume, upload/download, ls/cat,
// rm/mv, config-set and anything not on this short list -- is user-caused.
// A blank line (length 0) is user-caused: nothing legitimate sends one, and
// treating it as automatic would be a silent way to probe without ever
// taking control.
Traffic classify_command_line(const char* line, std::size_t length);

// A file-transfer start (PTYPE_FILE_START) is always user-caused: both
// uploading and downloading a file are named explicitly as user actions.
// It never carries text to classify -- the packet's own type already
// settles it.
constexpr Traffic classify_file_transfer_start() { return Traffic::user_caused; }

// The caller's own snapshot of whether interactive motion is in progress
// right now, reduced from Kernel::get_state() and Player::is_playing() to
// exactly the three facts blocks_transfer() needs. See the change
// explanation ("Why RUN plus not-playing is exactly interactive motion")
// for why this is enough to tell a jog/probe/tool-change move apart from a
// running job without a Kernel dependency here.
struct MotionState {
  // Kernel::get_state() == RUN: the conveyor is not idle (and the spindle,
  // if that alone would otherwise read Idle). RUN alone is ambiguous
  // between a running job, a jog, an MDI move and an automatic tool-change
  // move -- job_playing disambiguates it.
  bool run = false;
  // Kernel::get_state() == HOME: a homing move in progress. HOME is its
  // own state, distinct from RUN, and always blocks -- there is no "home
  // while playing a job" case to exempt.
  bool homing = false;
  // Player::is_playing() (PublicData player_checksum/is_playing_checksum):
  // a job is currently playing. A running job never blocks a transfer,
  // whatever `run` says; neither does a paused job (SUSPEND is not `run`
  // at all, so it already can't block on its own).
  bool job_playing = false;
};

// True while a jog, probe, homing or tool-change *move* is in progress --
// the cases the design blocks a transfer for. False for a tool-change
// *wait*, a running job, a paused job, and ordinary idle/alarm/hold states,
// none of which the design blocks a transfer for.
bool blocks_transfer(const MotionState& state);

// One client's identity, as recorded by the identify handshake -- just
// enough to say who holds control and to compare a candidate against the
// current holder. `identified == false` (the default) means "not a real
// client" -- ControlToken::gate() never lets such a sender take control.
struct Identity {
  bool identified = false;
  uint64_t id = 0;
  Link link = Link::wifi;
  char name[max_name_length + 1] = {0};  // NUL-terminated
  uint8_t name_len = 0;
};

// Copies the identity fields out of a client-table entry. `client` may be
// nullptr (a slot that disappeared between being looked up and being
// gated) -- that reads as an unidentified Identity{}, the same as a client
// that simply hasn't said hello yet.
Identity identity_of(const Client* client);

// What handling one inbound message actually did, for the caller to act
// on: whether to refuse it outright (and not dispatch it at all -- the
// caller sends its own visible reply and skips THEKERNEL->dispatch_console_
// line()), and whether the holder changed, which means the caller should
// publish a control-changed event.
struct GateResult {
  bool refused = false;
  bool holder_changed = false;
};

// The control token itself. One instance for the whole machine (see
// shared_control_token() below) -- there is exactly one holder, never one
// per link, which is the whole point: USB and WiFi are peers under the
// same token (docs/research/connection-follows-me.md, section 4, "USB is a
// peer link").
class ControlToken {
 public:
  ControlToken() = default;

  bool has_holder() const { return holder_.identified; }
  const Identity& holder() const { return holder_; }

  // The single gate: called once, from the one place each link's own
  // command dispatch converges (WifiProvider::on_main_loop,
  // SerialConsole::on_main_loop), for every message about to be
  // dispatched. `sender` is the identity of whichever client sent it (an
  // unidentified sender is always let through unchanged: it cannot yet
  // hold control, and this firmware answers a hello before anything else
  // it sends can reach here). `traffic` is already classified by the
  // caller; `motion` is the caller's own snapshot of the machine right
  // now.
  //
  // - Automatic traffic, or traffic from whoever already holds control:
  //   passed through unchanged (`refused = false`, `holder_changed =
  //   false`).
  // - User-caused traffic from someone else, while blocks_transfer(motion)
  //   is true: refused (`refused = true`) -- the caller must not dispatch
  //   it. The holder does not change.
  // - User-caused traffic from someone else, otherwise: seizes control
  //   silently (`holder_changed = true`) and is then let through.
  GateResult gate(const Identity& sender, Traffic traffic, const MotionState& motion);

  // Clears the holder unconditionally, with no report of whether anything
  // changed -- for a fresh boot or a protocol switch, where every client's
  // identity is about to be reset anyway and there is nobody left to tell.
  void clear();

  // Frees control if `id` currently holds it -- a disconnect or a silent
  // drop, detected by reconcile_holder() below. Returns true if the holder
  // actually changed (the caller should publish a control-changed event
  // naming nobody), false if `id` wasn't the holder or nobody had control.
  bool release_if_holder(uint64_t id);

 private:
  Identity holder_;
};

// One token, shared by both links.
ControlToken& shared_control_token();

// Frees `token`'s holder if it is no longer present (and identified) in
// `table` -- the disconnect/silent-drop case (docs/research/connection-
// follows-me.md, section 4.1: "Disconnect. When the controlling controller
// disconnects ... control is free at once."). Returns true if control was
// actually freed, so the caller should publish a control-changed event
// naming nobody. Takes both by reference, rather than only reading the
// shared singletons, so it stays host-testable against a private
// ClientTable/ControlToken pair the same way the rest of this file is.
bool reconcile_holder(ControlToken& token, const ClientTable& table);

}  // namespace multiclient
