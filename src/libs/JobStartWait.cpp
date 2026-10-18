#include "JobStartWait.h"

#include <cstring>

#include "Hello.h"
#include "MakeraControl.h"
#include "PublicData.h"

namespace multiclient {

namespace {

constexpr uint32_t microseconds_per_second = 1000000;

// How many second ticks a held start waits after a file transfer. The
// first can come at once, from a tick that fell due during the transfer;
// it brings the WiFi client list up to date. The second makes sure at
// least one whole pass of the main loop, in which the links act on what
// they received, comes between the two. So the wait is at most two seconds.
constexpr uint8_t settle_ticks_after_transfer = 2;

// True if the first word of `line` (`length` bytes, starting at `word_at`)
// is exactly `word`.
bool first_word_is(const char* line, std::size_t length, std::size_t word_at, const char* word) {
  const std::size_t word_length = std::strlen(word);
  if (length - word_at < word_length || std::memcmp(line + word_at, word, word_length) != 0) return false;
  if (length - word_at == word_length) return true;
  const char next = line[word_at + word_length];
  return next == ' ' || next == '\t' || next == '\r' || next == '\n';
}

bool connected(const ClientTable& table, uint64_t id) {
  return table.find_wifi_by_id(id) >= 0 || table.usb_has_id(id);
}

// The features byte from the hello of the connected client with `id`, 0 if
// there is none.
uint8_t features_of(const ClientTable& table, uint64_t id) {
  const Client* client = table.wifi_at(table.find_wifi_by_id(id));
  if (client == nullptr && table.usb_has_id(id)) client = table.usb();
  return client != nullptr ? client->features : 0;
}

}  // namespace

void JobStartWait::configure(int limit_s) {
  if (limit_s < 0) limit_s = 0;
  if (limit_s > 255) limit_s = 255;
  limit_s_ = static_cast<uint8_t>(limit_s);
}

uint8_t JobStartWait::hello_ack_features() const {
  return enabled() ? hello_feature_job_start_wait | hello_feature_keep_waiting : 0;
}

bool JobStartWait::needs_wait(const ClientTable& table, const Identity& starter) const {
  if (!enabled() || !starter.identified) return false;
  for (std::size_t i = 0; i < max_wifi_clients; ++i) {
    const Client* client = table.wifi_at(static_cast<int>(i));
    if (client != nullptr && client->identified && (client->features & hello_feature_job_start_wait) &&
        client->id != starter.id) {
      return true;
    }
  }
  const Client* usb = table.usb();
  return usb != nullptr && usb->identified && (usb->features & hello_feature_job_start_wait) && usb->id != starter.id;
}

void JobStartWait::begin(const Identity& starter) {
  ++start_id_;
  if (start_id_ == 0) start_id_ = 1;
  starter_id_ = starter.id;
  started_us_ = 0;
  pending_ = true;
  hashing_ = true;
  hash_complete_ = false;
  cancel_requested_ = false;
  limit_ran_out_in_transfer_ = false;
  timed_out_ = false;
  keep_waiting_requested_ = false;
  settle_ticks_ = 0;
  hashed_size_ = 0;
  md5_ = MD5();
}

bool JobStartWait::hash_step(HashRead read, void* file, uint8_t* buffer, std::size_t length, uint32_t now_us) {
  if (!hashing() || read == nullptr || buffer == nullptr || length == 0) return false;
  const long count = read(file, buffer, length);
  if (count > 0) {
    md5_.update(buffer, static_cast<MD5::size_type>(count));
    hashed_size_ += static_cast<uint32_t>(count);
    return false;
  }
  if (count == 0) {
    md5_.finalize();
    hash_complete_ = true;
  }
  hashing_ = false;
  started_us_ = now_us;
  return true;
}

bool JobStartWait::copy_checksum(uint8_t* digest) const {
  if (!hash_complete_ || digest == nullptr) return false;
  md5_.bindigest(digest, 16);
  return true;
}

bool JobStartWait::awaited(const Client* client) const {
  return client != nullptr && client->identified && (client->features & hello_feature_job_start_wait) &&
         client->id != starter_id_ && client->job_start_ready != start_id_;
}

StartCheck JobStartWait::check(const ClientTable& table, uint32_t now_us) const {
  if (cancel_requested_) return StartCheck::cancelled;
  if (!connected(table, starter_id_)) return StartCheck::starter_left;
  if (settling()) return StartCheck::keep_waiting;
  uint64_t ids[max_wifi_clients + 1];
  if (not_ready(table, ids, max_wifi_clients + 1) == 0) return StartCheck::all_ready;
  if (timed_out_) return StartCheck::keep_waiting;
  if (!hashing() && seconds_left(now_us) == 0) {
    if (features_of(table, starter_id_) & hello_feature_keep_waiting) return StartCheck::timed_out;
    return limit_ran_out_in_transfer_ ? StartCheck::time_limit_in_transfer : StartCheck::time_limit;
  }
  return StartCheck::keep_waiting;
}

bool JobStartWait::keep_waiting(uint64_t sender_id, uint32_t now_us) {
  if (!pending_ || sender_id != starter_id_) return false;
  if (!hashing()) started_us_ = now_us;
  timed_out_ = false;
  limit_ran_out_in_transfer_ = false;
  return true;
}

bool JobStartWait::request_keep_waiting(uint64_t sender_id) {
  if (!pending_ || sender_id != starter_id_) return false;
  keep_waiting_requested_ = true;
  return true;
}

void JobStartWait::transfer_ended(uint32_t now_us) {
  if (!pending_) return;
  settle_ticks_ = settle_ticks_after_transfer;
  if (keep_waiting_requested_) {
    keep_waiting_requested_ = false;
    keep_waiting(starter_id_, now_us);
  }
  if (!hashing() && seconds_left(now_us) == 0) limit_ran_out_in_transfer_ = true;
}

void JobStartWait::second_tick() {
  if (settle_ticks_ != 0) --settle_ticks_;
}

uint8_t JobStartWait::seconds_left(uint32_t now_us) const {
  if (hashing()) return limit_s_;
  // Unsigned difference of two raw readings: correct across one wrap of the
  // counter, and the limit (at most 255 s) is far below a wrap's ~71 min.
  const uint32_t elapsed_us = now_us - started_us_;
  const uint32_t limit_us = static_cast<uint32_t>(limit_s_) * microseconds_per_second;
  if (elapsed_us >= limit_us) return 0;
  return static_cast<uint8_t>((limit_us - elapsed_us + microseconds_per_second - 1) / microseconds_per_second);
}

uint8_t JobStartWait::not_ready(const ClientTable& table, uint64_t* ids, std::size_t capacity) const {
  uint8_t count = 0;
  for (std::size_t i = 0; i < max_wifi_clients && count < capacity; ++i) {
    const Client* client = table.wifi_at(static_cast<int>(i));
    if (awaited(client)) ids[count++] = client->id;
  }
  if (count < capacity && awaited(table.usb())) ids[count++] = table.usb()->id;
  return count;
}

bool JobStartWait::mark_ready(Client& client, const uint8_t* payload, std::size_t length) const {
  if (!pending_ || !client.identified || payload == nullptr || length < 2) return false;
  const uint16_t start_id = static_cast<uint16_t>((payload[0] << 8) | payload[1]);
  if (start_id != start_id_) return false;
  client.job_start_ready = start_id;
  return true;
}

JobStartWait& shared_job_start_wait() {
  static JobStartWait wait;
  return wait;
}

bool refused_while_start_pending(const char* line, std::size_t length) {
  if (line == nullptr) return false;
  std::size_t at = 0;
  while (at < length && (line[at] == ' ' || line[at] == '\t')) ++at;
  if (at == length) return false;
  const char first = line[at];
  if (first >= 'A' && first <= 'Z') return true;
  if (first == '$') return at + 1 < length && (line[at + 1] == 'H' || line[at + 1] == 'J');
  static const char* const job_commands[] = {"play", "buffer", "goto", "suspend", "resume"};
  for (const char* word : job_commands) {
    if (first_word_is(line, length, at, word)) return true;
  }
  return false;
}

TransferFrame held_start_frame(uint8_t type, const uint8_t* data, std::size_t length) {
  if (type == PTYPE_JOB_START_READY) return TransferFrame::ready;
  if (type != PTYPE_CTRL_MULTI || data == nullptr) return TransferFrame::other;
  const char* line = reinterpret_cast<const char*>(data);
  std::size_t at = 0;
  while (at < length && (line[at] == ' ' || line[at] == '\t')) ++at;
  if (first_word_is(line, length, at, "abort")) return TransferFrame::cancel;
  if (first_word_is(line, length, at, "keep-waiting")) return TransferFrame::keep_waiting;
  return TransferFrame::other;
}

bool dropped_when_marked(uint8_t type, const uint8_t* data, std::size_t length) {
  if (type == PTYPE_FILE_START || type == PTYPE_AUTO_COMMAND) return true;
  if (type != PTYPE_CTRL_MULTI || makera::is_diagnostic_request(data, length)) return false;
  return held_start_frame(type, data, length) == TransferFrame::other;
}

}  // namespace multiclient
