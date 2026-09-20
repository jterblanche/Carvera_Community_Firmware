#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "libs/ClientTable.h"
#include "libs/Hello.h"

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

std::vector<uint8_t> hello_payload(uint8_t version, uint64_t id, const std::string& name, uint8_t link) {
  std::vector<uint8_t> payload;
  payload.push_back(version);
  for (int i = 7; i >= 0; --i) payload.push_back(static_cast<uint8_t>(id >> (8 * i)));
  payload.push_back(static_cast<uint8_t>(name.size()));
  for (char c : name) payload.push_back(static_cast<uint8_t>(c));
  payload.push_back(link);
  return payload;
}

}  // namespace

int main() {
  {
    TEST("a well-formed hello parses id, name and link");
    const auto payload = hello_payload(1, 0x0102030405060708ULL, "Office PC", 0);
    multiclient::Hello hello;
    CHECK(multiclient::parse_hello(payload.data(), payload.size(), hello));
    CHECK(hello.id == 0x0102030405060708ULL);
    CHECK(hello.name_len == 9);
    CHECK(std::string(hello.name) == "Office PC");
    CHECK(hello.link == multiclient::Link::wifi);
  }

  {
    TEST("a hello with an empty name parses");
    const auto payload = hello_payload(1, 1, "", 0);
    multiclient::Hello hello;
    CHECK(multiclient::parse_hello(payload.data(), payload.size(), hello));
    CHECK(hello.name_len == 0);
    CHECK(std::string(hello.name).empty());
  }

  {
    TEST("link byte 1 parses as usb");
    const auto payload = hello_payload(1, 1, "USB", 1);
    multiclient::Hello hello;
    CHECK(multiclient::parse_hello(payload.data(), payload.size(), hello));
    CHECK(hello.link == multiclient::Link::usb);
  }

  {
    TEST("a payload shorter than the minimum shape is rejected");
    const std::vector<uint8_t> payload = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // 10 bytes, needs 11
    multiclient::Hello hello;
    CHECK(!multiclient::parse_hello(payload.data(), payload.size(), hello));
  }

  {
    TEST("a payload too short for its own name_len is rejected");
    auto payload = hello_payload(1, 1, "Office PC", 0);
    payload.resize(payload.size() - 2);  // truncate the name and the link byte
    multiclient::Hello hello;
    CHECK(!multiclient::parse_hello(payload.data(), payload.size(), hello));
  }

  {
    TEST("a name_len over the limit is rejected");
    std::vector<uint8_t> payload = {1, 0, 0, 0, 0, 0, 0, 0, 0, 32};  // name_len = 32
    payload.resize(payload.size() + 32 + 1, 'x');
    multiclient::Hello hello;
    CHECK(!multiclient::parse_hello(payload.data(), payload.size(), hello));
  }

  {
    TEST("an unrecognised protocol version is rejected");
    const auto payload = hello_payload(2, 1, "Office PC", 0);
    multiclient::Hello hello;
    CHECK(!multiclient::parse_hello(payload.data(), payload.size(), hello));
  }

  {
    TEST("trailing bytes past the link field are ignored, not rejected");
    auto payload = hello_payload(1, 1, "Office PC", 0);
    payload.push_back(0xAA);
    payload.push_back(0xBB);
    multiclient::Hello hello;
    CHECK(multiclient::parse_hello(payload.data(), payload.size(), hello));
    CHECK(hello.name_len == 9);
  }

  {
    TEST("build_hello_ack writes protocol_version, result and mode");
    uint8_t ack[multiclient::hello_ack_length];
    const std::size_t len =
        multiclient::build_hello_ack(ack, multiclient::hello_result_old_controller_present, multiclient::hello_mode_single_user);
    CHECK(len == 3);
    CHECK(ack[0] == 1);
    CHECK(ack[1] == multiclient::hello_result_old_controller_present);
    CHECK(ack[2] == multiclient::hello_mode_single_user);
  }

  {
    TEST("an empty table's client-list reply has a zero count and no entries");
    multiclient::ClientTable table;
    uint8_t out[multiclient::max_client_list_reply_length];
    const std::size_t len = multiclient::build_client_list_reply(table, out, sizeof(out));
    CHECK(len == 1);
    CHECK(out[0] == 0);
  }

  {
    TEST("client-list reply lists only identified clients, WiFi then USB");
    multiclient::ClientTable table;
    multiclient::Address addr_a;
    addr_a.ip[0] = addr_a.ip[1] = addr_a.ip[2] = addr_a.ip[3] = 1;
    addr_a.port = 1;
    multiclient::Address addr_b;
    addr_b.ip[0] = addr_b.ip[1] = addr_b.ip[2] = addr_b.ip[3] = 2;
    addr_b.port = 2;
    const int a = table.add_wifi(addr_a, 0);
    const int b = table.add_wifi(addr_b, 0);  // left unidentified
    (void)b;
    multiclient::set_identity(*table.wifi_at(a), 0xAAULL, "Alice", 5);
    table.set_usb_present(true, 0);
    multiclient::set_identity(*table.usb(), 0xBBULL, "Bob", 3);

    uint8_t out[multiclient::max_client_list_reply_length];
    const std::size_t len = multiclient::build_client_list_reply(table, out, sizeof(out));

    // Hand-decode: count(1) + entries of id(8)+name_len(1)+name+link(1)+has_control(1).
    CHECK(out[0] == 2);
    std::size_t offset = 1;

    uint64_t id = 0;
    for (int i = 0; i < 8; ++i) id = (id << 8) | out[offset + i];
    offset += 8;
    CHECK(id == 0xAAULL);
    const uint8_t name_len = out[offset++];
    CHECK(name_len == 5);
    CHECK(std::memcmp(out + offset, "Alice", 5) == 0);
    offset += name_len;
    CHECK(out[offset++] == 0);  // wifi
    CHECK(out[offset++] == 0);  // has_control

    id = 0;
    for (int i = 0; i < 8; ++i) id = (id << 8) | out[offset + i];
    offset += 8;
    CHECK(id == 0xBBULL);
    const uint8_t usb_name_len = out[offset++];
    CHECK(usb_name_len == 3);
    CHECK(std::memcmp(out + offset, "Bob", 3) == 0);
    offset += usb_name_len;
    CHECK(out[offset++] == 1);  // usb
    CHECK(out[offset++] == 0);  // has_control

    CHECK(offset == len);
  }

  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
