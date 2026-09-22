#include <cstdint>
#include <cstdio>
#include <cstring>

#include "libs/ControlToken.h"

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

multiclient::Traffic classify(const char* line) {
  return multiclient::classify_command_line(line, std::strlen(line));
}

multiclient::Identity make_identity(uint64_t id, const char* name, multiclient::Link link = multiclient::Link::wifi) {
  multiclient::Identity identity;
  identity.identified = true;
  identity.id = id;
  identity.link = link;
  identity.name_len = static_cast<uint8_t>(std::strlen(name));
  std::memcpy(identity.name, name, identity.name_len);
  return identity;
}

}  // namespace

int main() {
  {
    TEST("classify_command_line: the named connect-time queries are automatic");
    CHECK(classify("model") == multiclient::Traffic::automatic);
    CHECK(classify("version") == multiclient::Traffic::automatic);
    CHECK(classify("ftype") == multiclient::Traffic::automatic);
    CHECK(classify("time") == multiclient::Traffic::automatic);
    CHECK(classify("time 1758000000") == multiclient::Traffic::automatic);
    CHECK(classify("get wcs") == multiclient::Traffic::automatic);
    CHECK(classify("  get   wcs") == multiclient::Traffic::automatic);
  }

  {
    TEST("classify_command_line: a same-prefix word is not the automatic one");
    CHECK(classify("timezone") == multiclient::Traffic::user_caused);
    CHECK(classify("models") == multiclient::Traffic::user_caused);
    CHECK(classify("get wcsx") == multiclient::Traffic::user_caused);
    CHECK(classify("get wcs extra") == multiclient::Traffic::user_caused);
  }

  {
    TEST("classify_command_line: everything else is user-caused");
    CHECK(classify("G91 G1 X10 F500") == multiclient::Traffic::user_caused);
    CHECK(classify("G38.2 Z-10") == multiclient::Traffic::user_caused);
    CHECK(classify("$H") == multiclient::Traffic::user_caused);
    CHECK(classify("M3 S1000") == multiclient::Traffic::user_caused);
    CHECK(classify("play test.nc") == multiclient::Traffic::user_caused);
    CHECK(classify("abort") == multiclient::Traffic::user_caused);
    CHECK(classify("suspend") == multiclient::Traffic::user_caused);
    CHECK(classify("resume") == multiclient::Traffic::user_caused);
    CHECK(classify("upload test.nc") == multiclient::Traffic::user_caused);
    CHECK(classify("download test.nc") == multiclient::Traffic::user_caused);
    CHECK(classify("ls") == multiclient::Traffic::user_caused);
    CHECK(classify("rm test.nc") == multiclient::Traffic::user_caused);
    CHECK(classify("get temp") == multiclient::Traffic::user_caused);
    CHECK(classify("") == multiclient::Traffic::user_caused);
    CHECK(classify("   ") == multiclient::Traffic::user_caused);
  }

  {
    TEST("classify_file_transfer_start is always user-caused");
    CHECK(multiclient::classify_file_transfer_start() == multiclient::Traffic::user_caused);
  }

  {
    TEST("blocks_transfer: homing always blocks");
    multiclient::MotionState state;
    state.homing = true;
    CHECK(multiclient::blocks_transfer(state));
    state.run = true;
    state.job_playing = true;
    CHECK(multiclient::blocks_transfer(state));  // homing wins regardless of run/job_playing
  }

  {
    TEST("blocks_transfer: run without a job playing blocks (jog/probe/MDI/ATC move)");
    multiclient::MotionState state;
    state.run = true;
    CHECK(multiclient::blocks_transfer(state));
  }

  {
    TEST("blocks_transfer: a running job never blocks");
    multiclient::MotionState state;
    state.run = true;
    state.job_playing = true;
    CHECK(!multiclient::blocks_transfer(state));
  }

  {
    TEST("blocks_transfer: idle, hold, wait, tool, alarm, suspend never block");
    multiclient::MotionState state;  // run=false, homing=false
    CHECK(!multiclient::blocks_transfer(state));
  }

  {
    TEST("identity_of: a null client reads as unidentified");
    const multiclient::Identity identity = multiclient::identity_of(nullptr);
    CHECK(!identity.identified);
  }

  {
    TEST("identity_of: an unidentified client reads as unidentified");
    multiclient::Client client;
    client.identified = false;
    const multiclient::Identity identity = multiclient::identity_of(&client);
    CHECK(!identity.identified);
  }

  {
    TEST("identity_of: copies id/link/name from an identified client");
    multiclient::Client client;
    multiclient::set_identity(client, 42, "Office PC", 9);
    client.link = multiclient::Link::usb;
    const multiclient::Identity identity = multiclient::identity_of(&client);
    CHECK(identity.identified);
    CHECK(identity.id == 42);
    CHECK(identity.link == multiclient::Link::usb);
    CHECK(identity.name_len == 9);
    CHECK(std::memcmp(identity.name, "Office PC", 9) == 0);
  }

  {
    TEST("gate: automatic traffic never moves the token, even with nobody holding it");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::GateResult result = token.gate(office, multiclient::Traffic::automatic, multiclient::MotionState{});
    CHECK(!result.refused);
    CHECK(!result.holder_changed);
    CHECK(!token.has_holder());
  }

  {
    TEST("gate: an unidentified sender never moves the token");
    multiclient::ControlToken token;
    const multiclient::Identity nobody;  // identified == false
    const multiclient::GateResult result =
        token.gate(nobody, multiclient::Traffic::user_caused, multiclient::MotionState{});
    CHECK(!result.refused);
    CHECK(!result.holder_changed);
    CHECK(!token.has_holder());
  }

  {
    TEST("gate: user-caused traffic seizes control silently when nothing blocks it");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::GateResult result = token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{});
    CHECK(!result.refused);
    CHECK(result.holder_changed);
    CHECK(token.has_holder());
    CHECK(token.holder().id == 1);
  }

  {
    TEST("gate: the current holder acting again does not re-report a change");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{});
    const multiclient::GateResult result = token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{});
    CHECK(!result.refused);
    CHECK(!result.holder_changed);
    CHECK(token.holder().id == 1);
  }

  {
    TEST("gate: automatic traffic from a non-holder never seizes control");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{});
    const multiclient::GateResult result =
        token.gate(workshop, multiclient::Traffic::automatic, multiclient::MotionState{});
    CHECK(!result.refused);
    CHECK(!result.holder_changed);
    CHECK(token.holder().id == 1);
  }

  {
    TEST("gate: a transfer during interactive motion is refused, holder unchanged");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{});

    multiclient::MotionState jogging;
    jogging.run = true;
    const multiclient::GateResult result = token.gate(workshop, multiclient::Traffic::user_caused, jogging);
    CHECK(result.refused);
    CHECK(!result.holder_changed);
    CHECK(token.holder().id == 1);
  }

  {
    TEST("gate: a transfer during a running job is NOT refused");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{});

    multiclient::MotionState playing;
    playing.run = true;
    playing.job_playing = true;
    const multiclient::GateResult result = token.gate(workshop, multiclient::Traffic::user_caused, playing);
    CHECK(!result.refused);
    CHECK(result.holder_changed);
    CHECK(token.holder().id == 2);
  }

  {
    TEST("gate: a transfer during a tool-change wait is NOT refused (not run, not homing)");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{});

    const multiclient::GateResult result =
        token.gate(workshop, multiclient::Traffic::user_caused, multiclient::MotionState{});  // TOOL/WAIT: run=false
    CHECK(!result.refused);
    CHECK(result.holder_changed);
    CHECK(token.holder().id == 2);
  }

  {
    TEST("gate: a transfer while homing is refused even if job_playing happens to be true");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{});

    multiclient::MotionState homing;
    homing.homing = true;
    homing.job_playing = true;
    const multiclient::GateResult result = token.gate(workshop, multiclient::Traffic::user_caused, homing);
    CHECK(result.refused);
    CHECK(!result.holder_changed);
  }

  {
    TEST("release_if_holder: frees control only when `id` is the current holder");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{});

    CHECK(!token.release_if_holder(2));  // not the holder: no-op
    CHECK(token.has_holder());
    CHECK(token.release_if_holder(1));
    CHECK(!token.has_holder());
    CHECK(!token.release_if_holder(1));  // already free: no-op, no double report
  }

  {
    TEST("clear: unconditionally empties the holder");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{});
    token.clear();
    CHECK(!token.has_holder());
  }

  {
    TEST("gate: reconnecting never takes control on its own (hello alone never reaches gate as user-caused)");
    // The identify handshake itself never calls gate() -- WifiProvider and
    // SerialConsole answer PTYPE_HELLO directly and never set
    // command_waiting for it (see receive_wifi_data()/process_makera_byte()).
    // This case documents that guarantee at the level this file can: a
    // reconnecting client's *next* automatic message (a status poll or
    // heartbeat) still does not seize control.
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{});
    token.release_if_holder(1);  // office disconnected

    const multiclient::Identity reconnected = make_identity(1, "Office");  // same id, reconnected
    const multiclient::GateResult result =
        token.gate(reconnected, multiclient::Traffic::automatic, multiclient::MotionState{});
    CHECK(!result.holder_changed);
    CHECK(!token.has_holder());
  }

  {
    TEST("reconcile_holder: does nothing while the holder is still present on WiFi");
    multiclient::ClientTable table;
    multiclient::ControlToken token;
    multiclient::Address addr;
    addr.port = 1;
    const int index = table.add_wifi(addr, 100);
    multiclient::set_identity(*table.wifi_at(index), 7, "Office", 6);

    token.gate(make_identity(7, "Office"), multiclient::Traffic::user_caused, multiclient::MotionState{});
    CHECK(!multiclient::reconcile_holder(token, table));
    CHECK(token.has_holder());
  }

  {
    TEST("reconcile_holder: does nothing while the holder is still present on USB");
    multiclient::ClientTable table;
    multiclient::ControlToken token;
    table.set_usb_present(true, 100);
    multiclient::set_identity(*table.usb(), 9, "USB", 3);

    token.gate(make_identity(9, "USB", multiclient::Link::usb), multiclient::Traffic::user_caused, multiclient::MotionState{});
    CHECK(!multiclient::reconcile_holder(token, table));
    CHECK(token.has_holder());
  }

  {
    TEST("reconcile_holder: frees control once the holder is gone from the table");
    multiclient::ClientTable table;
    multiclient::ControlToken token;
    multiclient::Address addr;
    addr.port = 1;
    const int index = table.add_wifi(addr, 100);
    multiclient::set_identity(*table.wifi_at(index), 7, "Office", 6);

    token.gate(make_identity(7, "Office"), multiclient::Traffic::user_caused, multiclient::MotionState{});
    table.remove_wifi(index);  // disconnected

    CHECK(multiclient::reconcile_holder(token, table));
    CHECK(!token.has_holder());
    CHECK(!multiclient::reconcile_holder(token, table));  // already free: no double report
  }

  {
    TEST("reconcile_holder: a no-op when nobody holds control");
    multiclient::ClientTable table;
    multiclient::ControlToken token;
    CHECK(!multiclient::reconcile_holder(token, table));
  }

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
