#include "Hello.h"

#include <cstring>

namespace multiclient {

namespace {

// Only protocol_version 1 is defined today.
constexpr uint8_t hello_protocol_version = 1;

// Appends one client-list entry (id + name_len + name + link + has_control)
// to `out` at `offset`, if it fits within `out_capacity`. Returns the new
// offset, unchanged if the entry did not fit.
std::size_t append_client_entry(const Client& client, uint8_t* out, std::size_t offset, std::size_t out_capacity) {
  const std::size_t entry_length = 8 + 1 + client.name_len + 1 + 1;
  if (offset + entry_length > out_capacity) return offset;

  for (int i = 0; i < 8; ++i) out[offset + i] = static_cast<uint8_t>(client.id >> (8 * (7 - i)));
  offset += 8;
  out[offset++] = client.name_len;
  if (client.name_len != 0) {
    std::memcpy(out + offset, client.name, client.name_len);
    offset += client.name_len;
  }
  out[offset++] = client.link == Link::usb ? 1 : 0;
  out[offset++] = 0;  // has_control: no control-token mechanism yet
  return offset;
}

}  // namespace

bool parse_hello(const uint8_t* payload, std::size_t length, Hello& out) {
  // Minimum shape: version(1) + id(8) + name_len(1) + name(0) + link(1).
  constexpr std::size_t minimum_length = 1 + 8 + 1 + 1;
  if (payload == nullptr || length < minimum_length) return false;
  if (payload[0] != hello_protocol_version) return false;

  uint64_t id = 0;
  for (int i = 0; i < 8; ++i) id = (id << 8) | payload[1 + i];

  const uint8_t name_len = payload[9];
  if (name_len > max_name_length) return false;
  const std::size_t full_length = minimum_length + name_len;
  if (length < full_length) return false;

  out.id = id;
  out.name_len = name_len;
  if (name_len != 0) std::memcpy(out.name, payload + 10, name_len);
  out.name[name_len] = '\0';
  out.link = (payload[10 + name_len] == 1) ? Link::usb : Link::wifi;
  return true;
}

std::size_t build_hello_ack(uint8_t* out, uint8_t result, uint8_t mode) {
  out[0] = hello_protocol_version;
  out[1] = result;
  out[2] = mode;
  return hello_ack_length;
}

std::size_t build_client_list_reply(const ClientTable& table, uint8_t* out, std::size_t out_capacity) {
  std::size_t offset = 1;  // reserve the count byte
  uint8_t count = 0;

  for (std::size_t i = 0; i < max_wifi_clients; ++i) {
    const Client* client = table.wifi_at(static_cast<int>(i));
    if (client == nullptr || !client->identified) continue;
    const std::size_t next = append_client_entry(*client, out, offset, out_capacity);
    if (next == offset) break;  // out of room
    offset = next;
    ++count;
  }

  const Client* usb = table.usb();
  if (usb != nullptr && usb->identified) {
    const std::size_t next = append_client_entry(*usb, out, offset, out_capacity);
    if (next != offset) {
      offset = next;
      ++count;
    }
  }

  out[0] = count;
  return offset;
}

}  // namespace multiclient
