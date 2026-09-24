#include "ControlToken.h"

#include <cstring>
#include <string>

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

// `length` without the one line ending ("\n" or "\r\n") the controller
// sends after a command, if there is one.
std::size_t without_line_ending(const char* text, std::size_t length) {
  if (length > 0 && text[length - 1] == '\n') --length;
  if (length > 0 && text[length - 1] == '\r') --length;
  return length;
}

// True if `text` is exactly the controller's download of config.txt, with
// or without the line ending it sends. Any other spelling of the path is
// refused, even one the card would resolve to the same file.
bool is_config_download(const char* text, std::size_t length) {
  static const char expected[] = "download /sd/config.txt";
  const std::size_t expected_length = sizeof(expected) - 1;
  length = without_line_ending(text, length);
  return length == expected_length && std::memcmp(text, expected, expected_length) == 0;
}

// The file the machine last named in an upload-finished or play-started
// event, or null before the first one. Allocated on the first
// announcement, so until then it costs only this pointer.
std::string* announced_file = nullptr;

// The character shift_parameter() (libs/utils.cpp) turns `c` into when
// the download command reads its path. The controller sends a space as
// 0x01 and ?, &, ! and ~ as 0x02 to 0x05; the firmware reads 0x03 back as
// '*', not '&'.
char decoded_path_char(char c) {
  switch (c) {
    case 0x01: return ' ';
    case 0x02: return '?';
    case 0x03: return '*';
    case 0x04: return '!';
    case 0x05: return '~';
    default: return c;
  }
}

// True if `text` is "download " followed by the announced file's path,
// encoded the way the controller encodes it, with or without the line
// ending. It matches only if the download command would open exactly that
// file: no other spelling of the path is accepted.
bool is_announced_file_download(const char* text, std::size_t length) {
  if (announced_file == nullptr || announced_file->empty()) return false;
  static const char prefix[] = "download ";
  const std::size_t prefix_length = sizeof(prefix) - 1;
  length = without_line_ending(text, length);
  if (length != prefix_length + announced_file->size()) return false;
  if (std::memcmp(text, prefix, prefix_length) != 0) return false;

  const char* path = text + prefix_length;
  // A leading quote would make shift_parameter() read a quoted path.
  if (path[0] == '"' || path[0] == '\'') return false;
  for (std::size_t i = 0; i < announced_file->size(); ++i) {
    // A space or tab sent as itself would end the path early.
    if (path[i] == ' ' || path[i] == '\t') return false;
    if (decoded_path_char(path[i]) != (*announced_file)[i]) return false;
  }
  return true;
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

  // Reading config values changes nothing, so a controller without control
  // may do it. config-get-all counts only without a file name: given one, it
  // reads that file from the card rather than the config.
  if (word_is(line, length, start, "config-get")) return Traffic::automatic;
  if (word_is(line, length, start, "config-get-all")) {
    std::size_t i = skip_spaces(line, length, end_of_word(line, length, start));
    while (i < length && word_is(line, length, i, "-e")) i = skip_spaces(line, length, end_of_word(line, length, i));
    if (i >= length) return Traffic::automatic;
  }

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

void remember_announced_file(const char* path, std::size_t length) {
  if (path == nullptr || length == 0) {
    if (announced_file != nullptr) announced_file->clear();
    return;
  }
  if (announced_file == nullptr) announced_file = new std::string();
  announced_file->assign(path, length);
}

AutomaticCommand classify_automatic_command(const uint8_t* payload, std::size_t length, bool machine_idle,
                                            bool from_usb) {
  if (payload == nullptr || length < 2) return AutomaticCommand::refuse;

  const char* text = reinterpret_cast<const char*>(payload + 1);
  const std::size_t text_length = length - 1;
  switch (payload[0]) {
    case 0:
      if (classify_command_line(text, text_length) == Traffic::automatic) return AutomaticCommand::console_command;
      // baud changes the speed of the USB link only, so it is allowed from
      // the controller on that link and refused from a WiFi one.
      if (from_usb && word_is(text, text_length, skip_spaces(text, text_length, 0), "baud"))
        return AutomaticCommand::console_command;
      return AutomaticCommand::refuse;
    case 1:
      if (is_config_download(text, text_length)) return AutomaticCommand::file_transfer_start;
      if (machine_idle && is_announced_file_download(text, text_length)) return AutomaticCommand::file_transfer_start;
      return AutomaticCommand::refuse;
    default:
      return AutomaticCommand::refuse;
  }
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
