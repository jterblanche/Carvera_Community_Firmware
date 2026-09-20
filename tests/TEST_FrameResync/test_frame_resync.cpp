#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "libs/CRC16.h"
#include "libs/FrameResync.h"
#include "libs/MakeraFrame.h"

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

std::vector<uint8_t> make_frame(uint8_t type, const char* data) {
  const auto* first = reinterpret_cast<const uint8_t*>(data);
  return make_frame(type, std::vector<uint8_t>(first, first + std::strlen(data)));
}

// A ten-byte plain-text probe, byte for byte what the Community Controller's
// protocols/detector.py sends before it knows which protocol a machine
// speaks. None of these bytes are the frame header (0x86 0x68), so every one
// is a header-search failure while the decoder has no header yet.
std::vector<uint8_t> probe() {
  const char* text = "echo echo\n";
  return std::vector<uint8_t>(text, text + std::strlen(text));
}

std::string packet_data(const makera::Packet& packet) {
  return std::string(reinterpret_cast<const char*>(packet.data), packet.data_length);
}

struct Decoder {
  makera::Packet packet{};
  makera::ResyncingDecoder resync{packet};
};

// Feeds every byte in `bytes` through `decoder`, one at a time, starting at
// `start_ms` and advancing `step_ms` per byte. Returns the last result that
// was not `incomplete`, or `incomplete` if every byte was. Records every
// `header_error` result's notify_pending status in `notifications`, in
// order, if non-null -- true means consume_notify_pending() returned true
// for that trip.
makera::ResyncResult feed(Decoder& decoder, const std::vector<uint8_t>& bytes, uint32_t start_ms = 10,
                           uint32_t step_ms = 0, std::vector<bool>* notifications = nullptr) {
  makera::ResyncResult result = makera::ResyncResult::incomplete;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const makera::ResyncResult next = decoder.resync.decode_byte(bytes[i], start_ms + static_cast<uint32_t>(i) * step_ms);
    if (next == makera::ResyncResult::header_error && notifications != nullptr) {
      notifications->push_back(decoder.resync.consume_notify_pending());
    }
    if (next != makera::ResyncResult::incomplete) result = next;
  }
  return result;
}

}  // namespace

int main() {
  {
    TEST("ten junk bytes then a valid frame decodes");
    Decoder decoder;
    std::vector<bool> notifications;
    const makera::ResyncResult junk_result = feed(decoder, probe(), 10, 0, &notifications);
    CHECK(junk_result == makera::ResyncResult::incomplete);
    CHECK(notifications.empty());  // below the 20-byte threshold: no trip yet

    const makera::ResyncResult frame_result = feed(decoder, make_frame(0xD3, "?"), 20, 0, &notifications);
    CHECK(frame_result == makera::ResyncResult::complete);
    CHECK(decoder.packet.type == 0xD3);
    CHECK(packet_data(decoder.packet) == "?");
  }

  {
    TEST("nineteen junk bytes then a valid frame still decodes (one short of the threshold)");
    Decoder decoder;
    std::vector<uint8_t> junk = probe();
    junk.resize(19, 'x');  // 19 bytes: one below max_header_errors
    std::vector<bool> notifications;
    feed(decoder, junk, 10, 0, &notifications);
    CHECK(notifications.empty());

    const makera::ResyncResult frame_result = feed(decoder, make_frame(0x71, ""), 20, 0, &notifications);
    CHECK(frame_result == makera::ResyncResult::complete);
    CHECK(notifications.empty());  // the frame's own first byte must not be byte 20 of junk
  }

  {
    TEST("three probes (30 junk bytes) then a status frame: the probe-then-frame sequence "
         "every controller's detector.py sends against this firmware");
    Decoder decoder;
    std::vector<bool> notifications;
    // Three separate probes, each its own feed() call, 0.1s apart -- mirrors
    // three separate TCP reads, not one contiguous buffer.
    feed(decoder, probe(), 0, 0, &notifications);
    feed(decoder, probe(), 100, 0, &notifications);
    feed(decoder, probe(), 200, 0, &notifications);
    CHECK(notifications.size() == 1);      // exactly one trip across all 30 junk bytes
    CHECK(notifications[0] == true);       // and it must be shown

    const makera::ResyncResult frame_result = feed(decoder, make_frame(0x71, ""), 300, 0, &notifications);
    CHECK(frame_result == makera::ResyncResult::complete);
    CHECK(decoder.packet.type == 0x71);
    CHECK(decoder.packet.data_length == 0);
    CHECK(notifications.size() == 1);  // the frame did not trip the threshold again
  }

  {
    TEST("junk split across many small chunks then a frame still decodes");
    Decoder decoder;
    std::vector<bool> notifications;
    const std::vector<uint8_t> junk = probe();  // 10 bytes, fed one at a time as its own "chunk"
    for (int rep = 0; rep < 3; ++rep) {
      for (uint8_t byte : junk) {
        std::vector<uint8_t> one_byte_chunk{byte};
        feed(decoder, one_byte_chunk, 10 + rep, 0, &notifications);
      }
    }
    CHECK(notifications.size() == 1);

    const makera::ResyncResult frame_result = feed(decoder, make_frame(0xA2, "G28"), 50, 0, &notifications);
    CHECK(frame_result == makera::ResyncResult::complete);
    CHECK(packet_data(decoder.packet) == "G28");
  }

  {
    TEST("a frame split across chunks, arriving right after junk, still decodes");
    Decoder decoder;
    std::vector<bool> notifications;
    std::vector<uint8_t> junk = probe();
    junk.resize(20, 'x');  // trips the threshold exactly once
    feed(decoder, junk, 10, 0, &notifications);
    CHECK(notifications.size() == 1);

    const std::vector<uint8_t> frame = make_frame(0xD3, "status");
    // Split mid-frame, as a TCP read boundary would: the header and length
    // in one chunk, the rest in a second.
    const std::vector<uint8_t> first_half(frame.begin(), frame.begin() + 4);
    const std::vector<uint8_t> second_half(frame.begin() + 4, frame.end());
    const makera::ResyncResult first_result = feed(decoder, first_half, 30, 0, &notifications);
    CHECK(first_result == makera::ResyncResult::incomplete);
    const makera::ResyncResult second_result = feed(decoder, second_half, 31, 0, &notifications);
    CHECK(second_result == makera::ResyncResult::complete);
    CHECK(packet_data(decoder.packet) == "status");
    CHECK(notifications.size() == 1);  // splitting the frame itself must not trip anything
  }

  {
    TEST("enough junk to trip the threshold more than once: notified once, "
         "and a frame after the second trip still decodes");
    Decoder decoder;
    std::vector<bool> notifications;
    std::vector<uint8_t> junk(45, 'x');  // two full trips (20 + 20) plus 5 more building toward a third
    feed(decoder, junk, 10, 0, &notifications);
    CHECK(notifications.size() == 2);
    CHECK(notifications[0] == true);   // shown on the first trip
    CHECK(notifications[1] == false);  // not shown again on the second

    const makera::ResyncResult frame_result = feed(decoder, make_frame(0x71, ""), 100, 0, &notifications);
    CHECK(frame_result == makera::ResyncResult::complete);
    CHECK(notifications.size() == 2);  // the frame after the second trip decodes cleanly
  }

  {
    TEST("reset() re-arms the once-per-connection notification, as a reused slot needs");
    Decoder decoder;
    std::vector<uint8_t> junk(20, 'x');
    std::vector<bool> notifications;
    feed(decoder, junk, 10, 0, &notifications);
    CHECK(notifications.size() == 1 && notifications[0] == true);

    decoder.resync.reset();  // WifiClientStream::clear(), as a new connection takes the slot

    notifications.clear();
    feed(decoder, junk, 10, 0, &notifications);
    CHECK(notifications.size() == 1 && notifications[0] == true);  // shown again for the new connection
  }

  {
    TEST("a header byte followed immediately by junk (not a real frame) still resynchronises");
    // 0x86 alone looks exactly like the start of a real header until the
    // second byte arrives; when it doesn't, the hunt must restart on the
    // very next byte, not get stuck.
    Decoder decoder;
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 25; ++i) {
      bytes.push_back(static_cast<uint8_t>(makera::header >> 8));  // 0x86, never followed by 0x68
      bytes.push_back('x');
    }
    std::vector<bool> notifications;
    feed(decoder, bytes, 10, 0, &notifications);
    CHECK(!notifications.empty());

    const makera::ResyncResult frame_result = feed(decoder, make_frame(0xFE, "ok"), 100, 0, &notifications);
    CHECK(frame_result == makera::ResyncResult::complete);
    CHECK(packet_data(decoder.packet) == "ok");
  }

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
