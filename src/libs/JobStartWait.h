#pragma once

#include <cstddef>
#include <cstdint>

#include "ClientTable.h"
#include "ControlToken.h"
#include "md5.h"

// The job-start wait: when a job is started while other identified
// controllers that take part are connected (their hello carried
// hello_feature_job_start_wait, libs/Hello.h), the machine holds the start
// until each of them says it has loaded the file, or the configured limit
// runs out, or the controller that started it sends start-now. The
// controller that started it can cancel with abort, and its leaving
// cancels the start. If that controller's hello carried
// hello_feature_keep_waiting, the limit running out does not start the
// job: the start stays held until that controller chooses (start-now,
// keep-waiting or abort), or until every awaited controller is ready. A
// controller that does not take part (an older one, or one that never
// identified) is never waited for, and with none of the
// others connected a start runs at once, exactly as it always has. A held
// start first hashes the file itself (hash_step()), a little on each pass
// of the main loop, so the controllers are told an MD5 that matches the
// file on the card whatever is stored beside it; the limit starts once
// that is done. Plain C++, no Kernel or mbed dependency, so it builds and
// runs on the host (see tests/TEST_JobStartWait). The player (modules/utils/player/Player.cpp)
// owns what happens to the job; this file only keeps the wait's state and
// answers its questions.
namespace multiclient {

// The reply to a command refused because a job start is being held.
constexpr char job_start_pending_reply[] = "error:Refused -- a job is about to start\r\n";

// multi_client.start_wait_s falls back to this when unset.
constexpr int default_start_wait_s = 30;

// The held file is hashed hash_chunk_bytes at a time, for up to
// hash_slice_us on each pass of the main loop, so status, WiFi, halt and
// abort are served between slices.
constexpr std::size_t hash_chunk_bytes = 256;
constexpr uint32_t hash_slice_us = 5000;

// Reads the next bytes of the held file into `buffer`, at most `length`.
// Returns how many it read, 0 at the end of the file, or -1 if the read
// failed.
typedef long (*HashRead)(void* file, uint8_t* buffer, std::size_t length);

// What check() found.
enum class StartCheck : uint8_t {
  keep_waiting,  // someone awaited is not ready and the limit has not run out
  all_ready,     // every awaited controller is ready, or none is left
  time_limit,    // the limit ran out first
  timed_out,     // the limit ran out first, and the starter chooses what happens next
  time_limit_in_transfer,  // the limit had already run out when a file transfer ended
  starter_left,  // the controller that started the job is no longer connected
  cancelled,     // an abort arrived during a file transfer (request_cancel())
};

// What a frame another controller sends during a file transfer is to a held
// start (see held_start_frame()).
enum class TransferFrame : uint8_t {
  other,         // discarded, as anything else sent during a transfer
  ready,         // a job-start ready message
  cancel,        // an abort command
  keep_waiting,  // a keep-waiting command
};

class JobStartWait {
 public:
  // Sets the limit from multi_client.start_wait_s, clamped to 0..255
  // seconds. 0 turns the wait off. Never configured (the Z1 build), it is
  // off.
  void configure(int limit_s);
  uint8_t limit_s() const { return limit_s_; }
  bool enabled() const { return limit_s_ != 0; }

  // The features byte for a hello ack: hello_feature_job_start_wait and
  // hello_feature_keep_waiting while the wait is on.
  uint8_t hello_ack_features() const;

  // True if a job started by `starter` must wait: the wait is on, `starter`
  // is identified, and some other identified controller connected right
  // now takes part.
  bool needs_wait(const ClientTable& table, const Identity& starter) const;

  // Starts holding a job started by `starter`, under a new start id that
  // is never 0. The hold begins by hashing the file (hashing()); the limit
  // starts when that ends.
  void begin(const Identity& starter);
  void end() { pending_ = false; }
  bool pending() const { return pending_; }

  // True while a start is held and its file is still being hashed.
  bool hashing() const { return pending_ && hashing_; }

  // Hashes the next chunk of the held file: reads up to `length` bytes into
  // `buffer` through `read` and adds them to the MD5. At the end of the
  // file, or if a read fails, hashing ends and the limit starts at `now_us`
  // (a raw us_ticker_read() reading). Returns true on the call that ends
  // it; does nothing and returns false while not hashing.
  bool hash_step(HashRead read, void* file, uint8_t* buffer, std::size_t length, uint32_t now_us);

  // After hashing ended at the end of the file: copies the file's MD5 (16
  // bytes) into `digest` and returns true. False while hashing, after a
  // failed read, or before any start. Kept after end() until the next
  // begin(), for the job's play-started event.
  bool copy_checksum(uint8_t* digest) const;
  // How many bytes have been hashed: the file's size once copy_checksum()
  // returns true.
  uint32_t hashed_size() const { return hashed_size_; }
  uint16_t start_id() const { return start_id_; }
  uint64_t starter_id() const { return starter_id_; }

  // Whether the held start should end now, and why. An abort taken during
  // a file transfer comes first, then the starter's leaving, then every
  // awaited controller being ready, then the limit, which never runs out
  // while the file is being hashed. Once the limit has run out, the answer
  // is timed_out rather than time_limit or time_limit_in_transfer if the
  // starter's hello carried hello_feature_keep_waiting, and after
  // mark_timed_out() the limit is not looked at again until keep_waiting().
  // For two second ticks after a file transfer, the start waits whatever
  // else is true (see transfer_ended()).
  // A controller is awaited while it is connected, identified and takes
  // part, is not the starter, and has not said it is ready for this start;
  // one that connects during the wait is awaited too, without extending
  // the limit.
  StartCheck check(const ClientTable& table, uint32_t now_us) const;

  // A file transfer stops the main loop until it ends, so nothing is
  // decided while it runs, and what the links received meanwhile (a
  // controller that disconnected, an abort or ready message from USB, or
  // from WiFi during a USB transfer) has not been handled yet when it ends.
  // Called when a transfer ends, at `now_us`: while a start is held, it is
  // not started until second_tick() has been called twice more, so that
  // every link has handled what arrived and the WiFi client list has been
  // brought up to date first. A cancel is not held back. A keep-waiting
  // taken during the transfer (request_keep_waiting()) restarts the limit
  // here. Also records whether the limit had then already run out
  // (time_limit_in_transfer).
  void transfer_ended(uint32_t now_us);
  // Called once a second while no transfer runs.
  void second_tick();
  // True while a held start waits after a file transfer.
  bool settling() const { return pending_ && settle_ticks_ != 0; }

  // Records an abort taken during a file transfer from a controller the
  // control gate lets abort: the next check() cancels the held start.
  void request_cancel() {
    if (pending_) cancel_requested_ = true;
  }

  // Called once check() has answered timed_out: the start is now held for
  // the starter's choice, and check() stops answering timed_out.
  void mark_timed_out() {
    if (pending_) timed_out_ = true;
  }
  // True while a held start waits for the starter's choice.
  bool timed_out() const { return pending_ && timed_out_; }

  // keep-waiting from the controller with id `sender_id`, at `now_us`:
  // taken only from the starter while a start is held. Restarts the limit
  // in full (while hashing, the limit has not started and still starts when
  // hashing ends) and ends timed_out(). Returns true if it was taken.
  bool keep_waiting(uint64_t sender_id, uint32_t now_us);
  // The same keep-waiting, taken during a file transfer: recorded, and
  // applied when the transfer ends (transfer_ended()). Returns true if it
  // was taken.
  bool request_keep_waiting(uint64_t sender_id);

  // Whole seconds left before the limit, rounded up; 0 once it has run out.
  // The whole limit while the file is being hashed.
  uint8_t seconds_left(uint32_t now_us) const;

  // Writes the ids of the awaited controllers still not ready into `ids`,
  // WiFi slots in order then USB, at most `capacity` of them. Returns how
  // many it wrote.
  uint8_t not_ready(const ClientTable& table, uint64_t* ids, std::size_t capacity) const;

  // Records a ready message from `client`: payload start_id(2, BE), any
  // later bytes ignored. Taken only while a start is held, from an
  // identified client, for the current start id. True if it was taken.
  bool mark_ready(Client& client, const uint8_t* payload, std::size_t length) const;

 private:
  bool awaited(const Client* client) const;

  uint8_t limit_s_ = 0;
  bool pending_ = false;
  bool hashing_ = false;
  bool hash_complete_ = false;
  uint16_t start_id_ = 0;
  // Here, where they fill the padding before starter_id_.
  bool timed_out_ = false;
  bool keep_waiting_requested_ = false;
  uint64_t starter_id_ = 0;
  uint32_t started_us_ = 0;
  uint32_t hashed_size_ = 0;
  MD5 md5_;
  // Last, where they fill the padding at the end of the object.
  uint8_t settle_ticks_ = 0;
  bool cancel_requested_ = false;
  bool limit_ran_out_in_transfer_ = false;
};

// The one wait the player and both transports share.
JobStartWait& shared_job_start_wait();

// True for a command line, already stripped of its framing, that must not
// run while a job start is held: anything that would move the machine or
// change the held job. That is every G-code, M-code or tool line (the
// first character after any spaces is an upper-case letter), homing and
// jogging ($H, $J), and the console commands that act on a job: play,
// buffer, goto, suspend and resume. Everything else runs as usual,
// including start-now, keep-waiting and abort, reads, and the download a
// waiting controller needs.
bool refused_while_start_pending(const char* line, std::size_t length);

// While a WiFi file transfer runs, what another controller sends is
// discarded (libs/TransferBusy.h). While a start is held, three kinds of
// frame are kept instead, so the held start does not miss them: a job-start
// ready message (PTYPE_JOB_START_READY), and an abort or a keep-waiting (a
// PTYPE_CTRL_MULTI command line whose first word is abort or keep-waiting).
// `type`, `data` and `length` are the frame's type and payload.
TransferFrame held_start_frame(uint8_t type, const uint8_t* data, std::size_t length);

// Whether a frame marked during a file transfer (makera::take_frames()) is
// dropped when the rest of it is read after the transfer. It is if it is a
// command its sender may have been told the machine was busy for, so the
// command is not run after that answer: a command line (PTYPE_CTRL_MULTI),
// a file transfer start or an automatic command. An abort or a keep-waiting
// is still run, so a held start does not miss it, and so is a diagnose
// request, which gets no busy reply. Frames of any other type (a ready
// message, a halt or stop, a hello, a heartbeat, ...) are run as usual: the
// busy reply does not stand as their answer, and some, like a halt, must
// not be missed.
bool dropped_when_marked(uint8_t type, const uint8_t* data, std::size_t length);

}  // namespace multiclient
