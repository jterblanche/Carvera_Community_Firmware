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
// Multi-user mode: control moves only when the holder releases it or
// disconnects. A non-holder's command is refused, naming the holder, unless
// it is one the configured passive-rights level allows -- and an allowed
// one never takes control, whoever sends it. See Mode, PassiveRights and
// PassiveAction below, and ControlToken::gate().
namespace multiclient {

// Whether the machine is configured for single-user or multi-user control.
// A machine config setting (multi_client.mode); single-user is the default
// and reproduces every byte of today's behaviour -- see gate() below.
enum class Mode : uint8_t { single_user, multi_user };

// How much a non-holder may do in multi-user mode while someone else holds
// control, from a machine config setting (multi_client.passive_rights).
// Ordered so a higher level always includes what a lower one allows --
// gate() compares these numerically. watch_stop_upload is the
// default.
enum class PassiveRights : uint8_t {
  watch_only = 0,
  watch_stop = 1,
  watch_stop_upload = 2,
};

// The config.txt spellings of the three levels, and of the two modes,
// defined once so the two parse sites (SerialConsole and WifiProvider) and
// the tests cannot drift apart.
//
// The length limit is why these names are what they are. A config value is
// stored in a fixed char[CONFIGVALUE_MAX_LEN] and anything longer is
// truncated on read, so a name that does not fit does not fail -- it
// silently becomes a different, unrecognised string and falls back to
// watch_only, the most restrictive level. The earlier spelling
// "watch_pause_stop_upload" was 23 characters and could not be configured
// at all. The static_assert below turns a repeat of that mistake into a
// compile error. Keep it in step with CONFIGVALUE_MAX_LEN in
// libs/ConfigValue.h, which this header deliberately does not include: it
// is built for the host tests too, with no firmware headers available.
constexpr size_t config_value_max_chars = 19;  // CONFIGVALUE_MAX_LEN - 1

constexpr const char *mode_single_user = "single_user";
constexpr const char *mode_multi_user = "multi_user";
constexpr const char *rights_watch_only = "watch_only";
constexpr const char *rights_watch_stop = "watch_stop";
constexpr const char *rights_watch_stop_upload = "watch_stop_upload";

static_assert(sizeof("single_user") - 1 <= config_value_max_chars,
              "multi_client.mode value too long to be stored in config");
static_assert(sizeof("multi_user") - 1 <= config_value_max_chars,
              "multi_client.mode value too long to be stored in config");
static_assert(sizeof("watch_only") - 1 <= config_value_max_chars,
              "multi_client.passive_rights value too long to be stored in config");
static_assert(sizeof("watch_stop") - 1 <= config_value_max_chars,
              "multi_client.passive_rights value too long to be stored in config");
static_assert(sizeof("watch_stop_upload") - 1 <= config_value_max_chars,
              "multi_client.passive_rights value too long to be stored in config");

// A command classified against the passive-rights levels above: `pause`
// (`suspend`), `stop` (`abort`) or `upload` (`upload`), or `none` for
// everything else. Only meaningful in multi-user mode, and only for
// deciding whether a non-holder's command executes without taking control
// -- see classify_passive_action() and ControlToken::gate(). The realtime
// pause/resume/stop bytes (^X/^Y/!/~) never reach this: they are matched
// and acted on inline before dispatch, never through this text path.
enum class PassiveAction : uint8_t { none, pause, stop, upload };

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
// Reading config values is automatic too: `config-get`, and `config-get-all`
// with no file name (`-e` only).
// Every other command word -- every G-code line, MDI, jog, probe, homing,
// spindle, override, play/abort/suspend/resume, upload/download, ls/cat,
// rm/mv, config-set and every other config command that writes or reloads,
// and anything not on this short list -- is user-caused.
// A blank line (length 0) is user-caused: nothing legitimate sends one, and
// treating it as automatic would be a silent way to probe without ever
// taking control.
Traffic classify_command_line(const char* line, std::size_t length);

// A file-transfer start (PTYPE_FILE_START) is always user-caused: both
// uploading and downloading a file are named explicitly as user actions.
// It never carries text to classify -- the packet's own type already
// settles it.
constexpr Traffic classify_file_transfer_start() { return Traffic::user_caused; }

// Classifies one command line, already stripped of wire framing, the same
// way classify_command_line() does, but against the passive-rights actions
// instead: `suspend` (pause), `abort` (stop) and `upload`, matched as
// exactly the first word, the same as classify_command_line()'s allow-list.
// Everything else, including a blank line, is `PassiveAction::none`. Called
// on the same text classify_command_line() sees, for a PTYPE_FILE_START
// packet too (its payload is the same "upload <path>"/"download <path>"
// text an ordinary command carries) -- only used in multi-user mode, and
// only once the sender is already known not to be the current holder.
PassiveAction classify_passive_action(const char* line, std::size_t length);

// What to do with one automatic-command frame (PTYPE_AUTO_COMMAND in
// libs/PublicData.h). Its payload is a kind byte -- 0 for a console
// command, 1 for a file-transfer start -- followed by the text the ordinary
// frame type would carry. A console command runs only if
// classify_command_line() calls it automatic. A file-transfer start runs
// only if it is the download of /sd/config.txt, the controller's
// connect-time fetch of the machine's settings, or, while `machine_idle`
// (Kernel::get_state() == IDLE), the download of the file the machine last
// announced (remember_announced_file()), which a controller without control
// fetches to show the job. Everything else, including an unknown kind or an
// empty command, is refused and not executed. A command that runs skips the
// gate: it never moves control and is never refused for lack of it, in
// either mode.
enum class AutomaticCommand : uint8_t { refuse, console_command, file_transfer_start };

AutomaticCommand classify_automatic_command(const uint8_t* payload, std::size_t length, bool machine_idle);

// Remembers `path` (`length` bytes) as the file the machine last named to
// every controller in an upload-finished or play-started event, replacing
// the one before. It is kept until the next such event or a reboot. A
// null path or a length of 0 forgets it.
void remember_announced_file(const char* path, std::size_t length);

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
  // Kernel::get_state() == IDLE, exactly -- not merely "not running and not
  // homing", which is also true of ALARM, HOLD, SUSPEND, WAIT and TOOL.
  // Only used for the passive-rights "upload while idle" level
  // (PassiveRights::watch_stop_upload): a passive upload is allowed
  // only while nothing else -- including a paused job -- is going on.
  bool idle = false;
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

// Why gate() refused a message, so the caller can choose the right visible
// reply without re-deriving the reason itself.
enum class RefusalReason : uint8_t {
  none,               // not refused
  not_holder,         // multi-user mode, someone else holds control: name them
  motion_in_progress, // single-user mode, or control free: a jog/probe/homing/
                      // tool-change move is in progress
};

// What handling one inbound message actually did, for the caller to act
// on: whether to refuse it outright (and not dispatch it at all -- the
// caller sends its own visible reply and skips THEKERNEL->dispatch_console_
// line()), and whether the holder changed, which means the caller should
// publish a control-changed event.
struct GateResult {
  bool refused = false;
  bool holder_changed = false;
  RefusalReason reason = RefusalReason::none;
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
  // `mode`, `action` and `rights` all default to their single-user-mode
  // values, so a caller that never passes them -- every existing call, and
  // every existing test -- gets exactly today's single-user behaviour,
  // unchanged. `action` is the caller's own classify_passive_action() of
  // the same text `traffic` was classified from; `rights` is the
  // configured PassiveRights level.
  //
  // - Automatic traffic, or traffic from whoever already holds control:
  //   passed through unchanged (`refused = false`, `holder_changed =
  //   false`), in either mode.
  // - Multi-user mode, someone else already holds control: a privileged
  //   action (passive_action_allowed() in the .cpp says exactly which
  //   action/rights combinations qualify) is let through without moving
  //   control; anything else is refused (`reason = not_holder`), whatever
  //   the machine is doing.
  // - Single-user mode, or multi-user mode with control free, user-caused
  //   traffic from someone else: refused (`reason = motion_in_progress`)
  //   while blocks_transfer(motion) is true; otherwise seizes control
  //   silently (`holder_changed = true`) and is let through. This is
  //   today's single-user rule, applied here to free control too -- "the
  //   next user-caused message from anywhere takes it" once the holder has
  //   released or disconnected.
  GateResult gate(const Identity& sender, Traffic traffic, const MotionState& motion, Mode mode = Mode::single_user,
                   PassiveAction action = PassiveAction::none, PassiveRights rights = PassiveRights::watch_only);

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
