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
// cancels the start. A controller that does not take part (an older one,
// or one that never identified) is never waited for, and with none of the
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
  starter_left,  // the controller that started the job is no longer connected
};

class JobStartWait {
 public:
  // Sets the limit from multi_client.start_wait_s, clamped to 0..255
  // seconds. 0 turns the wait off. Never configured (the Z1 build), it is
  // off.
  void configure(int limit_s);
  uint8_t limit_s() const { return limit_s_; }
  bool enabled() const { return limit_s_ != 0; }

  // The features byte for a hello ack: hello_feature_job_start_wait while
  // the wait is on.
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

  // Whether the held start should end now, and why. The starter's leaving
  // comes first, then every awaited controller being ready, then the
  // limit, which never runs out while the file is being hashed. A
  // controller is awaited while it is connected, identified and takes
  // part, is not the starter, and has not said it is ready for this start;
  // one that connects during the wait is awaited too, without extending
  // the limit.
  StartCheck check(const ClientTable& table, uint32_t now_us) const;

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
  uint64_t starter_id_ = 0;
  uint32_t started_us_ = 0;
  uint32_t hashed_size_ = 0;
  MD5 md5_;
};

// The one wait the player and both transports share.
JobStartWait& shared_job_start_wait();

// True for a command line, already stripped of its framing, that must not
// run while a job start is held: anything that would move the machine or
// change the held job. That is every G-code, M-code or tool line (the
// first character after any spaces is an upper-case letter), homing and
// jogging ($H, $J), and the console commands that act on a job: play,
// buffer, goto, suspend and resume. Everything else runs as usual,
// including start-now and abort, reads, and the download a waiting
// controller needs.
bool refused_while_start_pending(const char* line, std::size_t length);

}  // namespace multiclient
