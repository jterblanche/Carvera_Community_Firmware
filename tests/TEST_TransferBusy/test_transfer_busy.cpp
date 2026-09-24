#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "libs/CRC16.h"
#include "libs/MakeraFrame.h"
#include "libs/PublicData.h"
#include "libs/TransferBusy.h"

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

std::vector<uint8_t> text(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::vector<uint8_t> join(std::initializer_list<std::vector<uint8_t>> parts) {
  std::vector<uint8_t> out;
  for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
  return out;
}

bool needs_reply(const std::vector<uint8_t>& read) { return multiclient::needs_busy_reply(read.data(), read.size()); }

multiclient::Address address(uint8_t last, uint16_t port) {
  multiclient::Address a;
  a.ip[0] = 192;
  a.ip[1] = 168;
  a.ip[2] = 1;
  a.ip[3] = last;
  a.port = port;
  return a;
}

const std::vector<uint8_t> query = make_frame(PTYPE_CTRL_SINGLE, {'?'});
const std::vector<uint8_t> keep_alive = make_frame(PTYPE_CTRL_SINGLE, {'Z' - 'A' + 1});
const std::vector<uint8_t> heartbeat = make_frame(PTYPE_HEARTBEAT, {});
const std::vector<uint8_t> diagnose = make_frame(PTYPE_CTRL_MULTI, text("diagnose\n"));

}  // namespace

int main() {
  {
    TEST("the busy reply is one text frame carrying the busy message");
    uint8_t frame[multiclient::busy_reply_frame_size];
    const std::size_t length = multiclient::build_busy_reply_frame(frame, sizeof(frame));
    CHECK(length == sizeof(frame));

    makera::Packet packet;
    makera::FrameDecoder decoder(packet);
    makera::DecodeResult result = makera::DecodeResult::incomplete;
    std::size_t used = 0;
    while (used < length && result == makera::DecodeResult::incomplete) result = decoder.decode_byte(frame[used++], 0);
    CHECK(result == makera::DecodeResult::complete);
    CHECK(used == length);
    CHECK(packet.type == PTYPE_NORMAL_INFO);
    const std::string message(reinterpret_cast<const char*>(packet.data), packet.data_length);
    CHECK(message == "error:Busy -- a file transfer is in progress, retry when it finishes\r\n");
  }

  {
    TEST("the busy reply is not built into a buffer too small for it");
    uint8_t frame[multiclient::busy_reply_frame_size];
    CHECK(multiclient::build_busy_reply_frame(frame, sizeof(frame) - 1) == 0);
    CHECK(multiclient::build_busy_reply_frame(nullptr, sizeof(frame)) == 0);
  }

  {
    TEST("automatic traffic alone needs no reply of its own");
    CHECK(!needs_reply(query));
    CHECK(!needs_reply(keep_alive));
    CHECK(!needs_reply(make_frame(PTYPE_CTRL_SINGLE, {'*'})));
    CHECK(!needs_reply(heartbeat));
    CHECK(!needs_reply(diagnose));
    CHECK(!needs_reply(join({query, heartbeat, query, diagnose})));
  }

  {
    TEST("a read with no frame header needs no reply of its own");
    CHECK(!needs_reply({}));
    CHECK(!needs_reply(text("some bytes from the middle of a frame")));
    CHECK(!needs_reply({0x86}));
    CHECK(!multiclient::needs_busy_reply(nullptr, 10));
  }

  {
    TEST("anything a person or a handshake causes needs a reply");
    CHECK(needs_reply(make_frame(PTYPE_HELLO, {1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0})));
    CHECK(needs_reply(make_frame(PTYPE_CTRL_MULTI, text("ls -s /sd/gcodes\n"))));
    CHECK(needs_reply(make_frame(PTYPE_CTRL_MULTI, text("diagnose extra\n"))));
    CHECK(needs_reply(make_frame(PTYPE_CTRL_SINGLE, {'Y' - 'A' + 1})));
    CHECK(needs_reply(make_frame(PTYPE_CTRL_SINGLE, {'X' - 'A' + 1})));
    CHECK(needs_reply(make_frame(PTYPE_CTRL_SINGLE, {'!'})));
    CHECK(needs_reply(make_frame(PTYPE_FILE_START, text("/sd/gcodes/a.nc"))));
    CHECK(needs_reply(make_frame(PTYPE_CLIENT_LIST_REQ, {})));
    CHECK(needs_reply(make_frame(PTYPE_CONTROL_RELEASE, {})));
    CHECK(needs_reply(make_frame(PTYPE_RELAY, text("x"))));
  }

  {
    TEST("a frame that needs a reply is found behind automatic traffic");
    CHECK(needs_reply(join({query, heartbeat, make_frame(PTYPE_HELLO, {1})})));
    CHECK(needs_reply(join({text("junk"), make_frame(PTYPE_CTRL_MULTI, text("M3\n"))})));
  }

  {
    TEST("a frame cut off by the end of the read needs a reply");
    const std::vector<uint8_t> hello = make_frame(PTYPE_HELLO, {1, 2, 3});
    CHECK(needs_reply(std::vector<uint8_t>(hello.begin(), hello.begin() + 4)));  // type not read yet
    CHECK(needs_reply(std::vector<uint8_t>(query.begin(), query.begin() + 5)));  // control byte not read yet
    CHECK(needs_reply(std::vector<uint8_t>(diagnose.begin(), diagnose.begin() + 9)));  // text not all read yet
  }

  {
    TEST("header bytes inside a skipped frame are not read as a new frame");
    // A heartbeat is header, length, type, CRC, footer. With 86 68 as its
    // CRC, reading on byte by byte would find a header there whose type
    // byte, four bytes on, is the 00 appended after the footer.
    std::vector<uint8_t> read = heartbeat;
    read[5] = 0x86;
    read[6] = 0x68;
    read.push_back(0x00);
    CHECK(!needs_reply(read));
  }

  {
    TEST("the first read from each client is answered whatever it holds");
    multiclient::BusyReplyLimiter limiter;
    CHECK(limiter.should_reply(address(10, 1000), false, 0));
    CHECK(limiter.should_reply(address(11, 1000), true, 0));
    CHECK(limiter.should_reply(address(10, 1001), false, 0));  // same host, another connection
  }

  {
    TEST("after the first reply, automatic traffic gets nothing more");
    multiclient::BusyReplyLimiter limiter;
    const multiclient::Address a = address(10, 1000);
    CHECK(limiter.should_reply(a, false, 0));
    CHECK(!limiter.should_reply(a, false, 1000000));
    CHECK(!limiter.should_reply(a, false, 60000000));
  }

  {
    TEST("a frame that needs a reply is answered after an automatic one, however soon");
    multiclient::BusyReplyLimiter limiter;
    const multiclient::Address a = address(10, 1000);
    CHECK(limiter.should_reply(a, false, 0));
    CHECK(limiter.should_reply(a, true, 1));
  }

  {
    TEST("frames that need a reply are answered no more often than every 250 ms");
    multiclient::BusyReplyLimiter limiter;
    const multiclient::Address a = address(10, 1000);
    CHECK(limiter.should_reply(a, true, 1000000));
    CHECK(!limiter.should_reply(a, true, 1000000 + multiclient::busy_reply_gap_us - 1));
    CHECK(limiter.should_reply(a, true, 1000000 + multiclient::busy_reply_gap_us));
    CHECK(!limiter.should_reply(a, true, 1000000 + multiclient::busy_reply_gap_us + 1));
    CHECK(!limiter.should_reply(a, false, 1000000 + 10 * multiclient::busy_reply_gap_us));
    CHECK(limiter.should_reply(a, true, 1000000 + 10 * multiclient::busy_reply_gap_us));
  }

  {
    TEST("the 250 ms gap holds across a wrap of the microsecond counter");
    multiclient::BusyReplyLimiter limiter;
    const multiclient::Address a = address(10, 1000);
    const uint32_t before_wrap = 0xFFFFFFFFu - 100000u;
    CHECK(limiter.should_reply(a, true, before_wrap));
    CHECK(!limiter.should_reply(a, true, before_wrap + 200000u));  // wrapped, 200 ms later
    CHECK(limiter.should_reply(a, true, before_wrap + multiclient::busy_reply_gap_us));
  }

  {
    TEST("each client has its own gap");
    multiclient::BusyReplyLimiter limiter;
    CHECK(limiter.should_reply(address(10, 1000), true, 0));
    CHECK(limiter.should_reply(address(11, 1000), true, 1));
    CHECK(!limiter.should_reply(address(10, 1000), true, 2));
    CHECK(!limiter.should_reply(address(11, 1000), true, 3));
  }

  {
    TEST("clearing starts over for the next transfer");
    multiclient::BusyReplyLimiter limiter;
    const multiclient::Address a = address(10, 1000);
    CHECK(limiter.should_reply(a, false, 0));
    CHECK(!limiter.should_reply(a, false, 1));
    limiter.clear();
    CHECK(limiter.should_reply(a, false, 2));
  }

  {
    TEST("a client beyond the tracked number reuses the oldest entry");
    multiclient::BusyReplyLimiter limiter;
    const std::size_t tracked = multiclient::max_wifi_clients + 1;
    for (std::size_t k = 0; k < tracked; ++k) {
      CHECK(limiter.should_reply(address(static_cast<uint8_t>(10 + k), 1000), false, 0));
    }
    for (std::size_t k = 0; k < tracked; ++k) {
      CHECK(!limiter.should_reply(address(static_cast<uint8_t>(10 + k), 1000), false, 1));
    }
    // One more client takes the first one's entry, so the first is answered
    // again as new and the second, still tracked, is not.
    CHECK(limiter.should_reply(address(99, 1000), false, 2));
    CHECK(limiter.should_reply(address(10, 1000), false, 3));
    CHECK(!limiter.should_reply(address(99, 1000), false, 4));
    CHECK(!limiter.should_reply(address(12, 1000), false, 5));
  }

  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
