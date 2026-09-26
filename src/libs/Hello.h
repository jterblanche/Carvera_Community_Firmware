#pragma once

#include <cstddef>
#include <cstdint>

#include "ClientTable.h"
#include "ControlToken.h"

// Wire encode/decode for the identify-handshake messages: hello, hello ack
// and client-list reply. Plain C++, no Kernel or mbed dependency, so it
// builds and runs on the host (see tests/TEST_Hello).
namespace multiclient {

// hello ack `result` values.
constexpr uint8_t hello_result_accepted = 0;
constexpr uint8_t hello_result_cap_reached = 1;
constexpr uint8_t hello_result_old_controller_present = 2;

// hello ack `mode` values, reporting the machine's configured
// multi_client.mode (ControlToken.h's Mode).
constexpr uint8_t hello_mode_single_user = 0;
constexpr uint8_t hello_mode_multi_user = 1;

constexpr std::size_t hello_ack_length = 3;

// A parsed hello payload.
struct Hello {
  uint64_t id = 0;
  char name[max_name_length + 1] = {0};  // NUL-terminated
  uint8_t name_len = 0;
  Link link = Link::wifi;
};

// Parses a hello payload: protocol_version(1) + id(8, big-endian) +
// name_len(1) + name(name_len) + link(1). Returns false, leaving `out`
// unspecified, when the payload is too short for its own name_len, when
// name_len exceeds max_name_length, or when protocol_version is not the one
// recognised value (1) -- the caller then treats the sender as unidentified,
// exactly as if nothing had arrived. Trailing bytes past the link field are
// ignored, so a future version's appended fields don't break this parser.
bool parse_hello(const uint8_t* payload, std::size_t length, Hello& out);

// Builds a hello-ack payload into `out` (must have room for
// hello_ack_length bytes). Returns hello_ack_length.
std::size_t build_hello_ack(uint8_t* out, uint8_t result, uint8_t mode);

// Longest possible client-list-reply payload: count(1) + up to
// (max_wifi_clients + 1) entries of at most 8+1+max_name_length+1+1 bytes
// each.
constexpr std::size_t max_client_list_reply_length = 1 + (max_wifi_clients + 1) * (8 + 1 + max_name_length + 1 + 1);

// Builds a client-list-reply payload listing every identified client in
// `table` (WiFi then USB) into `out` (capacity `out_capacity`, which should
// be at least max_client_list_reply_length to never truncate). An
// unidentified client is not listed -- it has no id or name to report.
// `has_control` is true for the one entry whose id matches `control`'s
// current holder, and false for every other entry -- including every entry
// when `control` has no holder at all. Returns the payload length.
std::size_t build_client_list_reply(const ClientTable& table, const ControlToken& control, uint8_t* out,
                                     std::size_t out_capacity);

// True while `client` has not identified but another client present has.
// Such a client is shown nothing of the machine and has nothing it sends
// acted on until it identifies: a controller that cannot identify sees a
// busy machine, as it would if the machine refused a second connection.
// False for nullptr, for an identified client, and whenever nobody present
// has identified, so a lone controller that never says hello is served
// exactly as before.
bool must_identify_first(const ClientTable& table, const Client* client);

// True for the frames still taken from a client that must identify first:
// its hello, and a status query ('?' as a realtime frame). Everything else
// from it is dropped unread.
bool taken_before_identifying(uint8_t type, const uint8_t* data, std::size_t length);

// True if `data`, about to be sent to a client that must identify first,
// may go to it: a hello ack, or a status frame with an empty payload. The
// empty status frame is how such a client's status query is answered. It
// shows no machine state, and controllers from before hello existed drop a
// status frame with nothing in it, but it is a valid frame, which is what
// a controller that knows hello waits for before sending its own.
bool sent_before_identifying(const uint8_t* data, std::size_t length);

}  // namespace multiclient
