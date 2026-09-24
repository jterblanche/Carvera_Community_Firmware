#include "ControlToken.h"

#include <cstring>

namespace multiclient {

namespace {

// True if `line[start..)` begins with `word`, followed immediately by the
// end of the line or whitespace -- i.e. `word` is exactly the first token,
// not merely a prefix of a longer one ("timex" must not match "time").
bool word_is(const char* line, std::size_t length, std::size_t start, const char* word) {
  const std::size_t word_len = std::strlen(word);
  if (start + word_len > length) return false;
  if (std::memcmp(line + start, word, word_len) != 0) return false;
  if (start + word_len == length) return true;
  const char next = line[start + word_len];
  return next == ' ' || next == '\t' || next == '\r' || next == '\n';
}

// Index of the first non-space/tab character at or after `start`, or
// `length` if there is none. shift_parameter() (SimpleShell.cpp), which
// every text command is already parsed with before dispatch, trims the
// same two characters.
std::size_t skip_spaces(const char* line, std::size_t length, std::size_t start) {
  std::size_t i = start;
  while (i < length && (line[i] == ' ' || line[i] == '\t')) ++i;
  return i;
}

// Index just past the current token starting at `start`.
std::size_t end_of_word(const char* line, std::size_t length, std::size_t start) {
  std::size_t i = start;
  while (i < length && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n') ++i;
  return i;
}

// True if `action`, from a non-holder in multi-user mode, is allowed to
// execute without taking control at the configured `rights` level. `pause`
// and `stop` are allowed from watch_stop up; `upload` needs the top
// level and the machine to be idle -- see MotionState::idle.
bool passive_action_allowed(PassiveAction action, PassiveRights rights, const MotionState& motion) {
  switch (action) {
    case PassiveAction::pause:
    case PassiveAction::stop:
      return static_cast<uint8_t>(rights) >= static_cast<uint8_t>(PassiveRights::watch_stop);
    case PassiveAction::upload:
      return static_cast<uint8_t>(rights) >= static_cast<uint8_t>(PassiveRights::watch_stop_upload) &&
             motion.idle;
    case PassiveAction::none:
    default:
      return false;
  }
}

}  // namespace

Traffic classify_command_line(const char* line, std::size_t length) {
  if (line == nullptr || length == 0) return Traffic::user_caused;

  const std::size_t start = skip_spaces(line, length, 0);
  if (start >= length) return Traffic::user_caused;

  if (word_is(line, length, start, "model")) return Traffic::automatic;
  if (word_is(line, length, start, "version")) return Traffic::automatic;
  if (word_is(line, length, start, "ftype")) return Traffic::automatic;
  if (word_is(line, length, start, "time")) return Traffic::automatic;

  if (word_is(line, length, start, "get")) {
    const std::size_t after_get = end_of_word(line, length, start);
    const std::size_t arg_start = skip_spaces(line, length, after_get);
    if (word_is(line, length, arg_start, "wcs")) {
      // Only when "wcs" is also the *last* token -- "get wcs foo" is not
      // the connect-time query this allow-list means.
      const std::size_t after_arg = end_of_word(line, length, arg_start);
      if (skip_spaces(line, length, after_arg) >= length) return Traffic::automatic;
    }
  }

  return Traffic::user_caused;
}

PassiveAction classify_passive_action(const char* line, std::size_t length) {
  if (line == nullptr || length == 0) return PassiveAction::none;

  const std::size_t start = skip_spaces(line, length, 0);
  if (start >= length) return PassiveAction::none;

  if (word_is(line, length, start, "suspend")) return PassiveAction::pause;
  if (word_is(line, length, start, "abort")) return PassiveAction::stop;
  if (word_is(line, length, start, "upload")) return PassiveAction::upload;

  return PassiveAction::none;
}

bool blocks_transfer(const MotionState& state) { return state.homing || (state.run && !state.job_playing); }

Identity identity_of(const Client* client) {
  Identity identity;
  if (client == nullptr || !client->identified) return identity;
  identity.identified = true;
  identity.id = client->id;
  identity.link = client->link;
  identity.name_len = client->name_len;
  std::memcpy(identity.name, client->name, sizeof(identity.name));
  return identity;
}

GateResult ControlToken::gate(const Identity& sender, Traffic traffic, const MotionState& motion, Mode mode,
                               PassiveAction action, PassiveRights rights) {
  GateResult result;

  // Automatic traffic is never gated, whoever sends it. This is checked
  // before anything else because a client of ours is not yet identified for
  // the first second or so of its session -- it sends hello only after its
  // own protocol probe -- and its status polling in that window must still
  // be answered.
  if (traffic == Traffic::automatic) return result;

  // An unidentified sender cannot hold control and never moves it. While
  // somebody else holds control its commands are refused, in either mode and
  // with no passive exemption: the machine cannot name it, cannot hold it to
  // account, and is about to drop it anyway. Before several clients could
  // connect at all, such a sender could only ever be the one client on the
  // machine, so nothing here narrows what a lone old controller may do --
  // with only it connected there is no holder and this refuses nothing.
  if (!sender.identified) {
    if (holder_.identified) {
      result.refused = true;
      result.reason = RefusalReason::not_holder;
    }
    return result;
  }

  // Already the holder: nothing changes, in either mode.
  if (holder_.identified && holder_.id == sender.id) return result;

  if (mode == Mode::multi_user && holder_.identified) {
    // Someone else holds control. Whatever the machine is doing, only a
    // privileged passive action executes -- and it never takes control,
    // whoever sends it (that is the whole point of it being passive).
    // Everything else is refused, naming the holder.
    if (passive_action_allowed(action, rights, motion)) return result;
    result.refused = true;
    result.reason = RefusalReason::not_holder;
    return result;
  }

  // Single-user mode, or multi-user mode with control free: today's rule.
  if (blocks_transfer(motion)) {
    result.refused = true;
    result.reason = RefusalReason::motion_in_progress;
    return result;
  }

  holder_ = sender;
  result.holder_changed = true;
  return result;
}

void ControlToken::clear() { holder_ = Identity{}; }

bool ControlToken::release_if_holder(uint64_t id) {
  if (!holder_.identified || holder_.id != id) return false;
  holder_ = Identity{};
  return true;
}

ControlToken& shared_control_token() {
  static ControlToken token;
  return token;
}

bool reconcile_holder(ControlToken& token, const ClientTable& table) {
  if (!token.has_holder()) return false;
  const uint64_t id = token.holder().id;
  if (table.find_wifi_by_id(id) >= 0) return false;
  if (table.usb_has_id(id)) return false;
  return token.release_if_holder(id);
}

}  // namespace multiclient
