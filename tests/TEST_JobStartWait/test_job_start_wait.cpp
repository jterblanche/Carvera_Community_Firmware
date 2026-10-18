#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "libs/CRC16.h"
#include "libs/ClientTable.h"
#include "libs/ControlToken.h"
#include "libs/FrameResync.h"
#include "libs/Hello.h"
#include "libs/JobStartWait.h"
#include "libs/MakeraFrame.h"
#include "libs/PublicData.h"

namespace {

int checks;
int failures;
const char* current_test;

#define CHECK(condition)                                                      \
  do {                                                                        \
    ++checks;                                                                 \
    if (!(condition)) {                                                       \
      ++failures;                                                             \
      std::printf("  FAIL (%s:%d) %s\n", current_test, __LINE__, #condition); \
    }                                                                         \
  } while (false)

#define TEST(name)     \
  current_test = name; \
  std::printf("%s\n", name)

using multiclient::Client;
using multiclient::ClientTable;
using multiclient::Identity;
using multiclient::JobStartWait;
using multiclient::StartCheck;

constexpr uint32_t second_us = 1000000;

multiclient::Address address(uint8_t last) {
  multiclient::Address a;
  a.ip[0] = 192;
  a.ip[1] = 168;
  a.ip[2] = 1;
  a.ip[3] = last;
  a.port = 5000;
  return a;
}

// Adds a WiFi client and identifies it with `id`, taking part in the wait
// when `takes_part`. Returns its slot index.
int add_identified(ClientTable& table, uint8_t last, uint64_t id, bool takes_part) {
  const int index = table.add_wifi(address(last), 0);
  Client* client = table.wifi_at(index);
  multiclient::set_identity(*client, id, "pc", 2);
  client->features = takes_part ? multiclient::hello_feature_job_start_wait : 0;
  return index;
}

void identify_usb(ClientTable& table, uint64_t id, bool takes_part) {
  table.set_usb_present(true, 0);
  table.start_usb_hello_window(0);
  Client* usb = table.usb();
  multiclient::set_identity(*usb, id, "usb", 3);
  usb->features = takes_part ? multiclient::hello_feature_job_start_wait : 0;
}

// Adds a WiFi client that takes part in the wait and, as a starter, is
// asked what to do when the limit runs out (hello_feature_keep_waiting).
int add_deciding(ClientTable& table, uint8_t last, uint64_t id) {
  const int index = add_identified(table, last, id, true);
  table.wifi_at(index)->features |= multiclient::hello_feature_keep_waiting;
  return index;
}

Identity identity(const ClientTable& table, int index) { return multiclient::identity_of(table.wifi_at(index)); }

// A ready message's payload for `start_id`: start_id(2, BE).
bool send_ready(const JobStartWait& wait, Client& client, uint16_t start_id) {
  const uint8_t payload[2] = {static_cast<uint8_t>(start_id >> 8), static_cast<uint8_t>(start_id)};
  return wait.mark_ready(client, payload, sizeof(payload));
}

bool refused(const char* line) { return multiclient::refused_while_start_pending(line, std::strlen(line)); }

multiclient::TransferFrame command_frame(const char* line) {
  return multiclient::held_start_frame(PTYPE_CTRL_MULTI, reinterpret_cast<const uint8_t*>(line), std::strlen(line));
}

// A whole Makera frame of `type` carrying `data`.
std::vector<uint8_t> make_frame(uint8_t type, const std::vector<uint8_t>& data) {
  const uint16_t length = static_cast<uint16_t>(data.size() + 3);
  std::vector<uint8_t> frame{static_cast<uint8_t>(makera::header >> 8), static_cast<uint8_t>(makera::header),
                             static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length), type};
  frame.insert(frame.end(), data.begin(), data.end());
  const uint16_t crc = crc16::ccitt(frame.data() + 2, length);
  frame.push_back(static_cast<uint8_t>(crc >> 8));
  frame.push_back(static_cast<uint8_t>(crc));
  frame.push_back(static_cast<uint8_t>(makera::footer >> 8));
  frame.push_back(static_cast<uint8_t>(makera::footer));
  return frame;
}

std::vector<uint8_t> command(const char* line) { return std::vector<uint8_t>(line, line + std::strlen(line)); }

// A file in memory for hash_step() to read, `chunk` bytes at most per read.
// Fails the read at byte `fail_at` when it is set. Counts the reads.
struct MemoryFile {
  const uint8_t* data = nullptr;
  std::size_t size = 0;
  std::size_t at = 0;
  std::size_t fail_at = SIZE_MAX;
  int reads = 0;
};

long read_memory(void* file, uint8_t* buffer, std::size_t length) {
  MemoryFile& memory = *static_cast<MemoryFile*>(file);
  ++memory.reads;
  if (memory.at >= memory.fail_at) return -1;
  std::size_t count = memory.size - memory.at;
  if (count > length) count = length;
  if (memory.at + count > memory.fail_at) count = memory.fail_at - memory.at;
  std::memcpy(buffer, memory.data + memory.at, count);
  memory.at += count;
  return static_cast<long>(count);
}

// Hashes `memory` to its end, `chunk` bytes per step, finishing at `now_us`.
// Returns how many steps it took, or -1 if hashing did not end.
int hash_all(JobStartWait& wait, MemoryFile& memory, std::size_t chunk, uint32_t now_us) {
  uint8_t buffer[multiclient::hash_chunk_bytes];
  for (int steps = 1; steps < 10000000; ++steps) {
    if (wait.hash_step(read_memory, &memory, buffer, chunk, now_us)) return steps;
    if (!wait.hashing()) return -1;
  }
  return -1;
}

// Holds a start by `starter` whose file (empty here) is hashed at once, so
// the limit starts at `now_us`.
void hold(JobStartWait& wait, const Identity& starter, uint32_t now_us) {
  wait.begin(starter);
  MemoryFile empty;
  hash_all(wait, empty, multiclient::hash_chunk_bytes, now_us);
}

std::string hex(const uint8_t* digest) {
  static const char digits[] = "0123456789abcdef";
  std::string text;
  for (int i = 0; i < 16; ++i) {
    text += digits[digest[i] >> 4];
    text += digits[digest[i] & 0x0f];
  }
  return text;
}

// The MD5 of `size` bytes at `data` as JobStartWait hashes it, `chunk`
// bytes per step, as hex; "" if it does not produce one.
std::string held_md5(const void* data, std::size_t size, std::size_t chunk) {
  JobStartWait wait;
  wait.configure(30);
  wait.begin(Identity{});
  MemoryFile memory;
  memory.data = static_cast<const uint8_t*>(data);
  memory.size = size;
  if (hash_all(wait, memory, chunk, 0) < 0) return "";
  uint8_t digest[16];
  if (!wait.copy_checksum(digest) || wait.hashed_size() != size) return "";
  return hex(digest);
}

}  // namespace

int main() {
  {
    TEST("the wait is off until configured, and off again at 0");
    JobStartWait wait;
    CHECK(!wait.enabled());
    CHECK(wait.limit_s() == 0);
    CHECK(wait.hello_ack_features() == 0);
    wait.configure(30);
    CHECK(wait.enabled());
    CHECK(wait.limit_s() == 30);
    CHECK(wait.hello_ack_features() ==
          (multiclient::hello_feature_job_start_wait | multiclient::hello_feature_keep_waiting));
    wait.configure(0);
    CHECK(!wait.enabled());
    CHECK(wait.hello_ack_features() == 0);
  }

  {
    TEST("the configured limit is clamped to 0..255 seconds, and defaults to 30");
    JobStartWait wait;
    wait.configure(-5);
    CHECK(wait.limit_s() == 0);
    wait.configure(1000);
    CHECK(wait.limit_s() == 255);
    CHECK(multiclient::default_start_wait_s == 30);
  }

  {
    TEST("needs_wait: only when another identified controller takes part");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    CHECK(!wait.needs_wait(table, identity(table, starter)));  // alone, even though it takes part

    add_identified(table, 2, 0x200, false);  // identified, without the feature
    CHECK(!wait.needs_wait(table, identity(table, starter)));

    table.add_wifi(address(3), 0);  // connected, never identified
    CHECK(!wait.needs_wait(table, identity(table, starter)));

    identify_usb(table, 0x300, true);
    CHECK(wait.needs_wait(table, identity(table, starter)));
  }

  {
    TEST("needs_wait: never while the wait is off, and never for an unidentified starter");
    ClientTable table;
    JobStartWait wait;
    const int starter = add_identified(table, 1, 0x100, false);
    add_identified(table, 2, 0x200, true);
    CHECK(!wait.needs_wait(table, identity(table, starter)));  // not configured
    wait.configure(30);
    CHECK(wait.needs_wait(table, identity(table, starter)));
    CHECK(!wait.needs_wait(table, Identity{}));
  }

  {
    TEST("needs_wait: a lone controller starts at once in either mode, on WiFi, USB or both");
    const multiclient::Mode modes[] = {multiclient::Mode::single_user, multiclient::Mode::multi_user};
    for (const multiclient::Mode mode : modes) {
      // The starter is whoever holds control after the start passes the
      // gate, as in Player::hold_start().
      ClientTable table;
      JobStartWait wait;
      wait.configure(30);
      multiclient::ControlToken token;
      const int pc = add_identified(table, 1, 0x100, true);
      token.gate(identity(table, pc), multiclient::Traffic::user_caused, multiclient::MotionState{}, mode);
      CHECK(token.holder().id == 0x100);
      CHECK(!wait.needs_wait(table, token.holder()));

      identify_usb(table, 0x100, true);  // the same controller on USB too
      CHECK(!wait.needs_wait(table, token.holder()));

      add_identified(table, 2, 0x200, false);  // an older controller
      const int stranger = table.add_wifi(address(3), 0);  // a connection that never said hello
      CHECK(!wait.needs_wait(table, token.holder()));

      CHECK(table.remove_wifi(stranger));     // make room: the table holds three WiFi links
      add_identified(table, 4, 0x400, true);  // a second controller that takes part
      CHECK(wait.needs_wait(table, token.holder()));

      ClientTable usb_only;
      multiclient::ControlToken usb_token;
      identify_usb(usb_only, 0x500, true);
      usb_token.gate(multiclient::identity_of(usb_only.usb()), multiclient::Traffic::user_caused,
                     multiclient::MotionState{}, mode);
      CHECK(usb_token.holder().id == 0x500);
      CHECK(!wait.needs_wait(usb_only, usb_token.holder()));
    }
  }

  {
    TEST("begin and end: pending, a non-zero start id that changes every start, and the starter");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    CHECK(!wait.pending());
    hold(wait, identity(table, starter), 0);
    CHECK(wait.pending());
    CHECK(wait.start_id() != 0);
    CHECK(wait.starter_id() == 0x100);
    const uint16_t first = wait.start_id();
    wait.end();
    CHECK(!wait.pending());
    hold(wait, identity(table, starter), 0);
    CHECK(wait.start_id() != first);
    wait.end();
  }

  {
    TEST("the start id never takes the value 0 when it wraps");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    bool saw_zero = false;
    for (uint32_t i = 0; i < 70000; ++i) {
      hold(wait, Identity{}, 0);
      if (wait.start_id() == 0) saw_zero = true;
    }
    CHECK(!saw_zero);
  }

  {
    TEST("check: waits for an awaited controller, then all_ready once it sends ready for this start");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 1000);
    CHECK(wait.check(table, 1000) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 1000 + 29 * second_us) == StartCheck::keep_waiting);
    CHECK(send_ready(wait, *table.wifi_at(other), wait.start_id()));
    CHECK(wait.check(table, 2000) == StartCheck::all_ready);
  }

  {
    TEST("check: time_limit once the limit has passed");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 5);
    CHECK(wait.check(table, 5 + 30 * second_us - 1) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 5 + 30 * second_us) == StartCheck::time_limit);
  }

  {
    TEST("check: the limit is measured correctly across a wrap of the microsecond counter");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    const uint32_t start = 0xFFFFFFFFu - 2 * second_us;
    hold(wait, identity(table, starter), start);
    CHECK(wait.check(table, start + 10 * second_us) == StartCheck::keep_waiting);
    CHECK(wait.seconds_left(start + 10 * second_us) == 20);
    CHECK(wait.check(table, start + 30 * second_us) == StartCheck::time_limit);
  }

  {
    TEST("seconds_left counts down in whole seconds, rounded up, and stops at 0");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    hold(wait, Identity{}, 0);
    CHECK(wait.seconds_left(0) == 30);
    CHECK(wait.seconds_left(1) == 30);
    CHECK(wait.seconds_left(second_us) == 29);
    CHECK(wait.seconds_left(29 * second_us + 1) == 1);
    CHECK(wait.seconds_left(30 * second_us) == 0);
    CHECK(wait.seconds_left(40 * second_us) == 0);
  }

  {
    TEST("check: starter_left once the starter is gone from the table, WiFi or USB");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 0);
    table.remove_wifi(starter);
    CHECK(wait.check(table, 0) == StartCheck::starter_left);

    ClientTable usb_table;
    identify_usb(usb_table, 0x300, true);
    add_identified(usb_table, 2, 0x200, true);
    hold(wait, multiclient::identity_of(usb_table.usb()), 0);
    CHECK(wait.check(usb_table, 0) == StartCheck::keep_waiting);
    usb_table.clear_usb_identity();
    CHECK(wait.check(usb_table, 0) == StartCheck::starter_left);
  }

  {
    TEST("check: the starter reconnecting under its own id is not the starter leaving");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 0);
    table.remove_wifi(starter);
    add_identified(table, 9, 0x100, true);
    CHECK(wait.check(table, 0) == StartCheck::keep_waiting);
  }

  {
    TEST("check: an awaited controller that leaves is no longer waited for");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 0);
    table.remove_wifi(other);
    CHECK(wait.check(table, 0) == StartCheck::all_ready);
  }

  {
    TEST("check: a controller that joins during the wait and takes part is waited for too");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 0);
    CHECK(send_ready(wait, *table.wifi_at(other), wait.start_id()));
    const int late = add_identified(table, 3, 0x300, true);
    CHECK(wait.check(table, 0) == StartCheck::keep_waiting);
    CHECK(send_ready(wait, *table.wifi_at(late), wait.start_id()));
    CHECK(wait.check(table, 0) == StartCheck::all_ready);
  }

  {
    TEST("check: controllers without the feature, and unidentified ones, never hold the start");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    add_identified(table, 3, 0x300, false);
    hold(wait, identity(table, starter), 0);
    table.set_usb_present(true, 0);
    table.start_usb_hello_window(0);
    CHECK(send_ready(wait, *table.wifi_at(other), wait.start_id()));
    CHECK(wait.check(table, 0) == StartCheck::all_ready);
  }

  {
    TEST("mark_ready: refused for another start's id, a short payload, an unidentified sender or no wait");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    Client& client = *table.wifi_at(other);
    CHECK(!send_ready(wait, client, 1));  // nothing is waiting
    hold(wait, identity(table, starter), 0);
    CHECK(!send_ready(wait, client, static_cast<uint16_t>(wait.start_id() + 1)));
    const uint8_t short_payload[1] = {0};
    CHECK(!wait.mark_ready(client, short_payload, sizeof(short_payload)));
    CHECK(!wait.mark_ready(client, nullptr, 2));
    const int stranger = table.add_wifi(address(7), 0);
    CHECK(!send_ready(wait, *table.wifi_at(stranger), wait.start_id()));
    CHECK(wait.check(table, 0) == StartCheck::keep_waiting);

    // A ready longer than two bytes is accepted: later fields may follow.
    const uint8_t longer[4] = {static_cast<uint8_t>(wait.start_id() >> 8), static_cast<uint8_t>(wait.start_id()), 9, 9};
    CHECK(wait.mark_ready(client, longer, sizeof(longer)));
    CHECK(wait.check(table, 0) == StartCheck::all_ready);
  }

  {
    TEST("a ready for one start does not count for the next");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 0);
    CHECK(send_ready(wait, *table.wifi_at(other), wait.start_id()));
    wait.end();
    hold(wait, identity(table, starter), 0);
    CHECK(wait.check(table, 0) == StartCheck::keep_waiting);
  }

  {
    TEST("not_ready lists the awaited controllers still loading, WiFi then USB, within capacity");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int ready = add_identified(table, 2, 0x200, true);
    add_identified(table, 3, 0x300, true);
    identify_usb(table, 0x400, true);
    hold(wait, identity(table, starter), 0);
    CHECK(send_ready(wait, *table.wifi_at(ready), wait.start_id()));
    uint64_t ids[multiclient::max_wifi_clients + 1] = {};
    CHECK(wait.not_ready(table, ids, sizeof(ids) / sizeof(ids[0])) == 2);
    CHECK(ids[0] == 0x300);
    CHECK(ids[1] == 0x400);
    CHECK(wait.not_ready(table, ids, 1) == 1);
    CHECK(ids[0] == 0x300);
  }

  {
    TEST("hash_step: the held file's MD5 matches the reference, empty, one chunk and many");
    CHECK(held_md5("", 0, multiclient::hash_chunk_bytes) == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(held_md5("abc", 3, multiclient::hash_chunk_bytes) == "900150983cd24fb0d6963f7d28e17f72");
    const char fox[] = "The quick brown fox jumps over the lazy dog";
    CHECK(held_md5(fox, sizeof(fox) - 1, multiclient::hash_chunk_bytes) == "9e107d9d372bb6826bd81d3542a419d6");
    CHECK(held_md5(fox, sizeof(fox) - 1, 1) == "9e107d9d372bb6826bd81d3542a419d6");
    CHECK(held_md5(fox, sizeof(fox) - 1, 7) == "9e107d9d372bb6826bd81d3542a419d6");

    uint8_t pattern[1000];
    for (std::size_t i = 0; i < sizeof(pattern); ++i) pattern[i] = static_cast<uint8_t>((i * 7 + 3) % 251);
    CHECK(held_md5(pattern, 512, multiclient::hash_chunk_bytes) == "9cef49878f04c5b1d59e97aa72710816");  // exactly two chunks
    CHECK(held_md5(pattern, sizeof(pattern), multiclient::hash_chunk_bytes) == "c90582fbbeb1275142915b85da61ade9");
    CHECK(held_md5(pattern, sizeof(pattern), 64) == "c90582fbbeb1275142915b85da61ade9");
    CHECK(held_md5(pattern, sizeof(pattern), 63) == "c90582fbbeb1275142915b85da61ade9");

    std::string million(1000000, 'a');
    CHECK(held_md5(million.data(), million.size(), multiclient::hash_chunk_bytes) == "7707d6ae4e027c70eea2a935c2296f21");
  }

  {
    TEST("hash_step: one read of at most one chunk per call, ending on the read that finds the end");
    JobStartWait wait;
    wait.configure(30);
    wait.begin(Identity{});
    uint8_t pattern[1000] = {};
    MemoryFile memory;
    memory.data = pattern;
    memory.size = sizeof(pattern);
    // 1000 bytes in 256-byte chunks: four reads with data, then one that
    // finds the end.
    CHECK(hash_all(wait, memory, multiclient::hash_chunk_bytes, 0) == 5);
    CHECK(memory.reads == 5);
    CHECK(wait.hashed_size() == 1000);
    CHECK(!wait.hashing());
    CHECK(wait.pending());
  }

  {
    TEST("hashing: a held start begins by hashing, and has no checksum until it ends");
    JobStartWait wait;
    wait.configure(30);
    CHECK(!wait.hashing());
    wait.begin(Identity{});
    CHECK(wait.pending());
    CHECK(wait.hashing());
    uint8_t digest[16];
    CHECK(!wait.copy_checksum(digest));
    const char text[] = "abc";
    MemoryFile memory;
    memory.data = reinterpret_cast<const uint8_t*>(text);
    memory.size = 3;
    uint8_t buffer[multiclient::hash_chunk_bytes];
    CHECK(!wait.hash_step(read_memory, &memory, buffer, sizeof(buffer), 0));
    CHECK(wait.hashing());
    CHECK(!wait.copy_checksum(digest));
    CHECK(wait.hash_step(read_memory, &memory, buffer, sizeof(buffer), 0));
    CHECK(!wait.hashing());
    CHECK(wait.copy_checksum(digest));
    CHECK(hex(digest) == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(wait.hashed_size() == 3);
  }

  {
    TEST("hashing: the limit does not run while the file is hashed, and starts when hashing ends");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    wait.begin(identity(table, starter));
    CHECK(wait.check(table, 0) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 100 * second_us) == StartCheck::keep_waiting);
    CHECK(wait.seconds_left(100 * second_us) == 30);
    std::string file(5000, 'x');
    MemoryFile memory;
    memory.data = reinterpret_cast<const uint8_t*>(file.data());
    memory.size = file.size();
    CHECK(hash_all(wait, memory, multiclient::hash_chunk_bytes, 200 * second_us) > 0);
    CHECK(wait.seconds_left(200 * second_us) == 30);
    CHECK(wait.check(table, 229 * second_us) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 230 * second_us) == StartCheck::time_limit);
  }

  {
    TEST("hashing: a failed read ends hashing without a checksum, and the limit starts");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    wait.begin(identity(table, starter));
    std::string file(5000, 'x');
    MemoryFile memory;
    memory.data = reinterpret_cast<const uint8_t*>(file.data());
    memory.size = file.size();
    memory.fail_at = 1000;
    CHECK(hash_all(wait, memory, multiclient::hash_chunk_bytes, 7 * second_us) > 0);
    CHECK(!wait.hashing());
    CHECK(wait.pending());
    uint8_t digest[16];
    CHECK(!wait.copy_checksum(digest));
    CHECK(wait.check(table, 36 * second_us) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 37 * second_us) == StartCheck::time_limit);
  }

  {
    TEST("hashing: a cancel during hashing stops it, and the next start hashes its own file from the start");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    wait.begin(identity(table, starter));
    std::string first(5000, 'x');
    MemoryFile partial;
    partial.data = reinterpret_cast<const uint8_t*>(first.data());
    partial.size = first.size();
    uint8_t buffer[multiclient::hash_chunk_bytes];
    CHECK(!wait.hash_step(read_memory, &partial, buffer, sizeof(buffer), 0));
    CHECK(!wait.hash_step(read_memory, &partial, buffer, sizeof(buffer), 0));
    wait.end();  // abort, a halt, or the starter leaving
    CHECK(!wait.pending());
    CHECK(!wait.hashing());
    const int reads = partial.reads;
    CHECK(!wait.hash_step(read_memory, &partial, buffer, sizeof(buffer), 0));
    CHECK(partial.reads == reads);  // nothing more is read once cancelled
    uint8_t digest[16];
    CHECK(!wait.copy_checksum(digest));

    wait.begin(identity(table, starter));
    CHECK(wait.hashing());
    CHECK(wait.hashed_size() == 0);
    const char text[] = "abc";
    MemoryFile second;
    second.data = reinterpret_cast<const uint8_t*>(text);
    second.size = 3;
    CHECK(hash_all(wait, second, multiclient::hash_chunk_bytes, 0) > 0);
    CHECK(wait.copy_checksum(digest));
    CHECK(hex(digest) == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(wait.hashed_size() == 3);
  }

  {
    TEST("hashing: the starter leaving, or nobody left to wait for, ends the hold while hashing");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    wait.begin(identity(table, starter));
    CHECK(wait.hashing());
    table.remove_wifi(starter);
    CHECK(wait.check(table, 0) == StartCheck::starter_left);

    ClientTable second;
    const int starter2 = add_identified(second, 1, 0x100, true);
    const int other2 = add_identified(second, 2, 0x200, true);
    wait.begin(identity(second, starter2));
    CHECK(wait.check(second, 0) == StartCheck::keep_waiting);
    second.remove_wifi(other2);
    CHECK(wait.check(second, 0) == StartCheck::all_ready);

    ClientTable third;
    const int starter3 = add_identified(third, 1, 0x100, true);
    const int other3 = add_identified(third, 2, 0x200, true);
    wait.begin(identity(third, starter3));
    CHECK(send_ready(wait, *third.wifi_at(other3), wait.start_id()));  // a controller giving up
    CHECK(wait.hashing());
    CHECK(wait.check(third, 0) == StartCheck::all_ready);
    (void)other;
  }

  {
    TEST("hashing: nothing is hashed without a held start");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    CHECK(!wait.needs_wait(table, identity(table, starter)));  // nobody to wait for: no hold
    CHECK(!wait.hashing());
    const char text[] = "abc";
    MemoryFile memory;
    memory.data = reinterpret_cast<const uint8_t*>(text);
    memory.size = 3;
    uint8_t buffer[multiclient::hash_chunk_bytes];
    CHECK(!wait.hash_step(read_memory, &memory, buffer, sizeof(buffer), 0));
    CHECK(memory.reads == 0);
    uint8_t digest[16];
    CHECK(!wait.copy_checksum(digest));
  }

  {
    TEST("hashing: the checksum is kept after the hold ends, for play-started, until the next start");
    JobStartWait wait;
    wait.configure(30);
    wait.begin(Identity{});
    const char text[] = "abc";
    MemoryFile memory;
    memory.data = reinterpret_cast<const uint8_t*>(text);
    memory.size = 3;
    CHECK(hash_all(wait, memory, multiclient::hash_chunk_bytes, 0) > 0);
    wait.end();
    uint8_t digest[16];
    CHECK(wait.copy_checksum(digest));
    CHECK(hex(digest) == "900150983cd24fb0d6963f7d28e17f72");
    wait.begin(Identity{});
    CHECK(!wait.copy_checksum(digest));
  }

  {
    TEST("refused_while_start_pending: G-code, M-code, tool, homing, jog, and the commands that act on a job");
    CHECK(refused("G0 X10"));
    CHECK(refused("  G1 X1 F100"));
    CHECK(refused("M3 S10000"));
    CHECK(refused("M32 part.nc"));
    CHECK(refused("M6T2"));
    CHECK(refused("T1"));
    CHECK(refused("$H"));
    CHECK(refused("$J X10 F500"));
    CHECK(refused("$J -c X"));
    CHECK(refused("play /sd/gcodes/part.nc"));
    CHECK(refused("play"));
    CHECK(refused("buffer M495 X0Y0P1"));
    CHECK(refused("goto 100"));
    CHECK(refused("suspend 5"));
    CHECK(refused("resume"));
  }

  {
    TEST("refused_while_start_pending: start-now, abort, reads, transfers and overrides still run");
    CHECK(!refused("start-now"));
    CHECK(!refused("keep-waiting"));
    CHECK(!refused("abort"));
    CHECK(!refused("ls -e -s /sd/gcodes"));
    CHECK(!refused("config-get sd wifi.max_clients"));
    CHECK(!refused("download /sd/gcodes/part.nc"));
    CHECK(!refused("md5sum /sd/gcodes/part.nc"));
    CHECK(!refused("progress"));
    CHECK(!refused("player"));
    CHECK(!refused("$X"));
    CHECK(!refused("$G"));
    CHECK(!refused("$F S120"));
    CHECK(!refused(""));
    CHECK(!refused("   "));
    CHECK(!multiclient::refused_while_start_pending(nullptr, 0));
    CHECK(!multiclient::refused_while_start_pending("G0", 0));
  }

  {
    TEST("held_start_frame: a ready message and an abort are kept during a transfer, nothing else");
    using multiclient::TransferFrame;
    const uint8_t ready[2] = {0, 1};
    CHECK(multiclient::held_start_frame(PTYPE_JOB_START_READY, ready, sizeof(ready)) == TransferFrame::ready);
    CHECK(command_frame("abort") == TransferFrame::cancel);
    CHECK(command_frame("abort\n") == TransferFrame::cancel);
    CHECK(command_frame("abort\r\n") == TransferFrame::cancel);
    CHECK(command_frame("  abort 1") == TransferFrame::cancel);
    CHECK(command_frame("aborted") == TransferFrame::other);
    CHECK(command_frame("keep-waiting") == TransferFrame::keep_waiting);
    CHECK(command_frame(" keep-waiting\r\n") == TransferFrame::keep_waiting);
    CHECK(command_frame("keep-waitingx") == TransferFrame::other);
    CHECK(command_frame("start-now") == TransferFrame::other);
    CHECK(command_frame("G0 X10") == TransferFrame::other);
    CHECK(command_frame("") == TransferFrame::other);
    CHECK(multiclient::held_start_frame(PTYPE_CTRL_MULTI, nullptr, 5) == TransferFrame::other);
    const uint8_t abort_bytes[] = {'a', 'b', 'o', 'r', 't'};
    CHECK(multiclient::held_start_frame(PTYPE_CTRL_SINGLE, abort_bytes, sizeof(abort_bytes)) == TransferFrame::other);
    CHECK(multiclient::held_start_frame(PTYPE_HEARTBEAT, nullptr, 0) == TransferFrame::other);
  }

  {
    TEST("dropped_when_marked: a command line, a file transfer start or an automatic command, "
         "but not an abort, a keep-waiting, a diagnose request or any other type");
    const auto dropped = [](uint8_t type, const char* text) {
      const std::vector<uint8_t> data = command(text);
      return multiclient::dropped_when_marked(type, data.data(), data.size());
    };
    CHECK(dropped(PTYPE_CTRL_MULTI, "download /sd/gcodes/part.nc\n"));
    CHECK(dropped(PTYPE_CTRL_MULTI, "start-now\n"));
    CHECK(dropped(PTYPE_CTRL_MULTI, "aborted\n"));
    CHECK(dropped(PTYPE_FILE_START, "/sd/gcodes/part.nc"));
    CHECK(dropped(PTYPE_FILE_START, ""));
    CHECK(dropped(PTYPE_AUTO_COMMAND, "M6T1"));
    CHECK(!dropped(PTYPE_CTRL_MULTI, "abort\n"));
    CHECK(!dropped(PTYPE_CTRL_MULTI, "keep-waiting\n"));
    CHECK(!dropped(PTYPE_CTRL_MULTI, "diagnose\n"));
    const uint8_t ready[2] = {0, 1};
    CHECK(!multiclient::dropped_when_marked(PTYPE_JOB_START_READY, ready, sizeof(ready)));
    const uint8_t halt[1] = {'X' - 'A' + 1};
    CHECK(!multiclient::dropped_when_marked(PTYPE_CTRL_SINGLE, halt, sizeof(halt)));
    CHECK(!multiclient::dropped_when_marked(PTYPE_HEARTBEAT, nullptr, 0));
    CHECK(!dropped(PTYPE_HELLO, "x"));
    CHECK(!dropped(PTYPE_RELAY, "x"));
  }

  {
    TEST("a frame cut off at the end of a transfer, nothing taken from the read, is dropped after the transfer "
         "only if it is a command, wherever the cut falls");
    struct Case {
      uint8_t type;
      std::vector<uint8_t> data;
      bool dropped;
    };
    const Case cases[] = {
        {PTYPE_JOB_START_READY, {0x12, 0x34}, false},
        {PTYPE_CTRL_MULTI, command("abort\n"), false},
        {PTYPE_CTRL_MULTI, command("keep-waiting\n"), false},
        {PTYPE_CTRL_SINGLE, {'X' - 'A' + 1}, false},
        {PTYPE_HEARTBEAT, {}, false},
        {PTYPE_CTRL_MULTI, command("download /sd/gcodes/part.nc\n"), true},
        {PTYPE_FILE_START, command("/sd/gcodes/part.nc"), true},
        {PTYPE_AUTO_COMMAND, command("M6T1"), true},
    };
    const auto take_nothing = [](const makera::Packet&) { return false; };
    for (const Case& c : cases) {
      const std::vector<uint8_t> frame = make_frame(c.type, c.data);
      for (std::size_t cut = 1; cut < frame.size(); ++cut) {
        makera::Packet packet{};
        makera::ResyncingDecoder decoder{packet};
        // The last read the transfer discards ends `cut` bytes into the frame.
        CHECK(!makera::take_frames(decoder, frame.data(), cut, 10, take_nothing));
        // The rest is read after the transfer, as receive_wifi_data() reads it.
        makera::ResyncResult result = makera::ResyncResult::incomplete;
        for (std::size_t i = cut; i < frame.size(); ++i) result = decoder.decode_byte(frame[i], 20);
        CHECK(result == makera::ResyncResult::marked);
        CHECK(multiclient::dropped_when_marked(packet.type, packet.data, packet.data_length) == c.dropped);
        CHECK(packet.type == c.type && packet.data_length == c.data.size() &&
              (c.data.empty() || std::memcmp(packet.data, c.data.data(), c.data.size()) == 0));
      }
    }
  }

  {
    TEST("cancel during a transfer: the start is cancelled when the transfer ends, even past the limit");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 0);
    wait.request_cancel();  // the starter's abort, taken while another controller downloads
    wait.transfer_ended(57 * second_us);
    CHECK(wait.settling());
    CHECK(wait.check(table, 57 * second_us) == StartCheck::cancelled);
    wait.second_tick();
    wait.second_tick();
    CHECK(wait.check(table, 59 * second_us) == StartCheck::cancelled);
  }

  {
    TEST("cancel during a transfer: comes before the starter leaving and every controller being ready");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 0);
    CHECK(send_ready(wait, *table.wifi_at(other), wait.start_id()));
    wait.request_cancel();
    table.remove_wifi(starter);
    CHECK(wait.check(table, 0) == StartCheck::cancelled);
  }

  {
    TEST("cancel request: ignored with no start held, and never carried into the next start");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    wait.request_cancel();
    hold(wait, identity(table, starter), 0);
    CHECK(wait.check(table, 0) == StartCheck::keep_waiting);
    wait.request_cancel();
    wait.end();
    hold(wait, identity(table, starter), 0);
    CHECK(wait.check(table, 0) == StartCheck::keep_waiting);
  }

  {
    TEST("starter gone during a transfer: nothing starts until the client list is brought up to date");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 0);
    // The starter disconnects during a transfer that ends past the limit;
    // the table still lists it until the WiFi second tick reconciles it.
    wait.transfer_ended(61 * second_us);
    CHECK(wait.check(table, 61 * second_us) == StartCheck::keep_waiting);
    wait.second_tick();
    CHECK(wait.check(table, 61 * second_us + 1000) == StartCheck::keep_waiting);
    table.remove_wifi(starter);
    CHECK(wait.check(table, 61 * second_us + 2000) == StartCheck::starter_left);
    wait.second_tick();
    CHECK(wait.check(table, 62 * second_us) == StartCheck::starter_left);
  }

  {
    TEST("starter gone during a transfer: a starter leaving is never held back by the wait after it");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 0);
    table.remove_wifi(starter);
    wait.transfer_ended(5 * second_us);
    CHECK(wait.settling());
    CHECK(wait.check(table, 5 * second_us) == StartCheck::starter_left);
  }

  {
    TEST("ready during a transfer: kept, and the start follows two second ticks after the transfer");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    const int third = add_identified(table, 3, 0x300, true);
    hold(wait, identity(table, starter), 0);
    // One watcher downloads; the other says ready while it does.
    CHECK(send_ready(wait, *table.wifi_at(third), wait.start_id()));
    wait.transfer_ended(20 * second_us);
    CHECK(send_ready(wait, *table.wifi_at(other), wait.start_id()));
    CHECK(wait.check(table, 20 * second_us) == StartCheck::keep_waiting);
    wait.second_tick();
    CHECK(wait.check(table, 20 * second_us + 500) == StartCheck::keep_waiting);
    wait.second_tick();
    CHECK(!wait.settling());
    CHECK(wait.check(table, 21 * second_us) == StartCheck::all_ready);
  }

  {
    TEST("time limit and transfers: time_limit_in_transfer only when the limit ran out before the transfer ended");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);

    hold(wait, identity(table, starter), 0);
    wait.transfer_ended(57 * second_us);
    wait.second_tick();
    CHECK(wait.check(table, 58 * second_us) == StartCheck::keep_waiting);
    wait.second_tick();
    CHECK(wait.check(table, 58 * second_us) == StartCheck::time_limit_in_transfer);
    wait.end();

    // A transfer that ended before the limit: the limit then runs out as usual.
    hold(wait, identity(table, starter), 0);
    CHECK(wait.check(table, 0) == StartCheck::keep_waiting);
    wait.transfer_ended(10 * second_us);
    wait.second_tick();
    wait.second_tick();
    CHECK(wait.check(table, 12 * second_us) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 30 * second_us) == StartCheck::time_limit);
    wait.end();

    // The limit does not run while the file is hashed, so a transfer that
    // ends during hashing never finds it run out.
    wait.begin(identity(table, starter));
    wait.transfer_ended(100 * second_us);
    wait.second_tick();
    wait.second_tick();
    CHECK(wait.check(table, 100 * second_us) == StartCheck::keep_waiting);
    MemoryFile empty;
    hash_all(wait, empty, multiclient::hash_chunk_bytes, 100 * second_us);
    CHECK(wait.check(table, 130 * second_us) == StartCheck::time_limit);
  }

  {
    TEST("transfers and ticks change nothing with no start held, and a new start does not wait");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    const int other = add_identified(table, 2, 0x200, true);
    wait.transfer_ended(0);
    CHECK(!wait.settling());
    wait.second_tick();
    hold(wait, identity(table, starter), 0);
    CHECK(!wait.settling());
    wait.transfer_ended(40 * second_us);
    CHECK(wait.settling());
    wait.end();
    CHECK(!wait.settling());
    hold(wait, identity(table, starter), 0);
    CHECK(!wait.settling());
    CHECK(send_ready(wait, *table.wifi_at(other), wait.start_id()));
    CHECK(wait.check(table, 0) == StartCheck::all_ready);
    CHECK(wait.check(table, 30 * second_us) == StartCheck::all_ready);
  }

  {
    TEST("timed out: a starter that can choose holds the start when the limit runs out, indefinitely");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_deciding(table, 1, 0x100);
    add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 5);
    CHECK(!wait.timed_out());
    CHECK(wait.check(table, 5 + 30 * second_us - 1) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 5 + 30 * second_us) == StartCheck::timed_out);
    wait.mark_timed_out();
    CHECK(wait.timed_out());
    CHECK(wait.pending());
    CHECK(wait.seconds_left(5 + 30 * second_us) == 0);
    CHECK(wait.check(table, 5 + 31 * second_us) == StartCheck::keep_waiting);
    // Hours later, past a wrap of the microsecond counter, it is still held.
    CHECK(wait.check(table, 5 + 3600 * second_us) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 5 + 2 * 3600 * second_us) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 4 * second_us) == StartCheck::keep_waiting);
    wait.end();
    CHECK(!wait.timed_out());
  }

  {
    TEST("timed out: a starter on USB that can choose is asked too");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    add_identified(table, 2, 0x200, true);
    identify_usb(table, 0x900, true);
    table.usb()->features |= multiclient::hello_feature_keep_waiting;
    hold(wait, multiclient::identity_of(table.usb()), 0);
    CHECK(wait.check(table, 30 * second_us) == StartCheck::timed_out);
  }

  {
    TEST("timed out: a starter that cannot choose still starts the job at the limit, as before");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_deciding(table, 2, 0x200);  // a watcher that could choose changes nothing
    hold(wait, identity(table, starter), 0);
    CHECK(wait.check(table, 30 * second_us) == StartCheck::time_limit);
    wait.end();
    hold(wait, identity(table, starter), 0);
    wait.transfer_ended(57 * second_us);
    wait.second_tick();
    wait.second_tick();
    CHECK(wait.check(table, 58 * second_us) == StartCheck::time_limit_in_transfer);
  }

  {
    TEST("timed out: the limit running out during a transfer holds the start for the starter's choice");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_deciding(table, 1, 0x100);
    add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 0);
    wait.transfer_ended(57 * second_us);
    wait.second_tick();
    CHECK(wait.check(table, 58 * second_us) == StartCheck::keep_waiting);
    wait.second_tick();
    CHECK(wait.check(table, 58 * second_us) == StartCheck::timed_out);
  }

  {
    TEST("timed out: every awaited controller becoming ready starts the job, with all_ready");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_deciding(table, 1, 0x100);
    const int other = add_identified(table, 2, 0x200, true);
    const int third = add_identified(table, 3, 0x300, true);
    hold(wait, identity(table, starter), 0);
    CHECK(wait.check(table, 30 * second_us) == StartCheck::timed_out);
    wait.mark_timed_out();
    CHECK(send_ready(wait, *table.wifi_at(other), wait.start_id()));
    CHECK(wait.check(table, 40 * second_us) == StartCheck::keep_waiting);
    table.remove_wifi(third);  // an awaited controller that leaves is not waited for
    CHECK(wait.check(table, 41 * second_us) == StartCheck::all_ready);
  }

  {
    TEST("timed out: the starter leaving or an abort taken in a transfer still cancels");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_deciding(table, 1, 0x100);
    add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 0);
    wait.mark_timed_out();
    wait.request_cancel();
    CHECK(wait.check(table, 31 * second_us) == StartCheck::cancelled);
    wait.end();
    hold(wait, identity(table, starter), 0);
    wait.mark_timed_out();
    table.remove_wifi(starter);
    CHECK(wait.check(table, 31 * second_us) == StartCheck::starter_left);
  }

  {
    TEST("keep_waiting: from the starter, restarts the whole limit and returns to waiting, every time");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_deciding(table, 1, 0x100);
    add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 0);
    CHECK(wait.check(table, 30 * second_us) == StartCheck::timed_out);
    wait.mark_timed_out();
    CHECK(wait.keep_waiting(0x100, 45 * second_us));
    CHECK(!wait.timed_out());
    CHECK(wait.seconds_left(45 * second_us) == 30);
    CHECK(wait.check(table, 75 * second_us - 1) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 75 * second_us) == StartCheck::timed_out);
    // Also taken while still waiting: the limit starts over from then.
    CHECK(wait.keep_waiting(0x100, 60 * second_us));
    CHECK(wait.seconds_left(61 * second_us) == 29);
    CHECK(wait.check(table, 89 * second_us) == StartCheck::keep_waiting);
    CHECK(wait.keep_waiting(0x100, 89 * second_us));
    CHECK(wait.check(table, 118 * second_us) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 119 * second_us) == StartCheck::timed_out);
  }

  {
    TEST("keep_waiting: a starter that cannot choose gets the full limit again, then the job starts");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_identified(table, 1, 0x100, true);
    add_identified(table, 2, 0x200, true);
    hold(wait, identity(table, starter), 0);
    CHECK(wait.keep_waiting(0x100, 20 * second_us));
    CHECK(wait.check(table, 49 * second_us) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 50 * second_us) == StartCheck::time_limit);
  }

  {
    TEST("keep_waiting: refused from any other controller, and with no start held");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_deciding(table, 1, 0x100);
    add_deciding(table, 2, 0x200);
    CHECK(!wait.keep_waiting(0x100, 0));
    CHECK(!wait.request_keep_waiting(0x100));
    hold(wait, identity(table, starter), 0);
    wait.mark_timed_out();
    CHECK(!wait.keep_waiting(0x200, 35 * second_us));
    CHECK(!wait.keep_waiting(0, 35 * second_us));
    CHECK(wait.timed_out());
    CHECK(!wait.request_keep_waiting(0x200));
    wait.transfer_ended(36 * second_us);
    CHECK(wait.timed_out());
    wait.end();
    CHECK(!wait.keep_waiting(0x100, 40 * second_us));
    CHECK(!wait.request_keep_waiting(0x100));
  }

  {
    TEST("keep_waiting: during hashing it is taken, and the limit still starts when hashing ends");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_deciding(table, 1, 0x100);
    add_identified(table, 2, 0x200, true);
    wait.begin(identity(table, starter));
    CHECK(wait.keep_waiting(0x100, 5 * second_us));
    CHECK(wait.hashing());
    MemoryFile empty;
    hash_all(wait, empty, multiclient::hash_chunk_bytes, 10 * second_us);
    CHECK(wait.seconds_left(10 * second_us) == 30);
    CHECK(wait.check(table, 40 * second_us) == StartCheck::timed_out);
  }

  {
    TEST("keep_waiting during a transfer: kept, and the limit starts over when the transfer ends");
    ClientTable table;
    JobStartWait wait;
    wait.configure(30);
    const int starter = add_deciding(table, 1, 0x100);
    add_identified(table, 2, 0x200, true);

    // Sent while waiting; the limit runs out before the transfer ends.
    hold(wait, identity(table, starter), 0);
    CHECK(wait.request_keep_waiting(0x100));
    wait.transfer_ended(57 * second_us);
    CHECK(wait.seconds_left(57 * second_us) == 30);
    wait.second_tick();
    wait.second_tick();
    CHECK(wait.check(table, 58 * second_us) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 87 * second_us) == StartCheck::timed_out);
    wait.end();

    // Sent while timed out.
    hold(wait, identity(table, starter), 0);
    wait.mark_timed_out();
    CHECK(wait.request_keep_waiting(0x100));
    CHECK(wait.timed_out());  // nothing changes until the transfer ends
    wait.transfer_ended(70 * second_us);
    CHECK(!wait.timed_out());
    CHECK(wait.seconds_left(70 * second_us) == 30);
    wait.second_tick();
    wait.second_tick();
    CHECK(wait.check(table, 99 * second_us) == StartCheck::keep_waiting);
    CHECK(wait.check(table, 100 * second_us) == StartCheck::timed_out);
    wait.end();

    // Never carried into the next start.
    hold(wait, identity(table, starter), 0);
    CHECK(wait.request_keep_waiting(0x100));
    wait.end();
    hold(wait, identity(table, starter), 0);
    wait.transfer_ended(57 * second_us);
    wait.second_tick();
    wait.second_tick();
    CHECK(wait.check(table, 58 * second_us) == StartCheck::timed_out);
    wait.end();

    // An abort kept in the same transfer still cancels.
    hold(wait, identity(table, starter), 0);
    CHECK(wait.request_keep_waiting(0x100));
    wait.request_cancel();
    wait.transfer_ended(57 * second_us);
    CHECK(wait.check(table, 57 * second_us) == StartCheck::cancelled);
  }

  {
    TEST("job_start_pending_reply: one line in the control gate's refusal form");
    const char* reply = multiclient::job_start_pending_reply;
    const std::size_t length = std::strlen(reply);
    const char prefix[] = "error:Refused -- ";
    CHECK(std::strncmp(reply, prefix, sizeof(prefix) - 1) == 0);
    CHECK(std::strcmp(reply + length - 2, "\r\n") == 0);
    CHECK(std::strpbrk(reply, "\r\n") == reply + length - 2);
  }

  {
    TEST("shared_job_start_wait returns the same object every time");
    CHECK(&multiclient::shared_job_start_wait() == &multiclient::shared_job_start_wait());
  }

  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
