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

multiclient::PassiveAction classify_passive(const char* line) {
  return multiclient::classify_passive_action(line, std::strlen(line));
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
    // The manual tool-change confirm (ATCHandler.cpp's M490 dispatch, "exit
    // tool change waiting status") is not on the automatic allow-list, so
    // it always classifies as user-caused, from any client.
    CHECK(classify("M490.2") == multiclient::Traffic::user_caused);
    CHECK(classify("M490.4") == multiclient::Traffic::user_caused);
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
    TEST("gate: an unidentified sender is refused while somebody else holds control (single-user)");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity nobody;  // identified == false
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{});
    CHECK(token.holder().id == 1);

    const multiclient::GateResult result =
        token.gate(nobody, multiclient::Traffic::user_caused, multiclient::MotionState{});
    CHECK(result.refused);
    CHECK(result.reason == multiclient::RefusalReason::not_holder);
    CHECK(!result.holder_changed);
    CHECK(token.holder().id == 1);
  }

  {
    TEST("gate: an unidentified sender is refused while somebody else holds control (multi-user)");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity nobody;
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user);
    CHECK(token.holder().id == 1);

    const multiclient::GateResult result =
        token.gate(nobody, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user);
    CHECK(result.refused);
    CHECK(result.reason == multiclient::RefusalReason::not_holder);
    CHECK(token.holder().id == 1);
  }

  {
    // No passive exemption for a sender the machine cannot name: a stop from
    // an unidentified client is refused even under rights that would let an
    // identified passive client send one.
    TEST("gate: an unidentified sender gets no passive exemption");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity nobody;
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user);

    const multiclient::GateResult result =
        token.gate(nobody, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user,
                   multiclient::PassiveAction::stop, multiclient::PassiveRights::watch_stop_upload);
    CHECK(result.refused);
    CHECK(result.reason == multiclient::RefusalReason::not_holder);
    CHECK(token.holder().id == 1);
  }

  {
    // One of ours is unidentified for the first second or so of its session,
    // until it sends hello. Its status polling must still be answered.
    TEST("gate: automatic traffic from an unidentified sender is still answered");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity nobody;
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user);

    const multiclient::GateResult result =
        token.gate(nobody, multiclient::Traffic::automatic, multiclient::MotionState{}, multiclient::Mode::multi_user);
    CHECK(!result.refused);
    CHECK(!result.holder_changed);
    CHECK(token.holder().id == 1);
  }

  {
    // The lone old controller: nobody holds control, so nothing is refused
    // and it works exactly as it did before several clients could connect.
    TEST("gate: a lone unidentified sender is refused nothing, in either mode");
    multiclient::ControlToken single;
    multiclient::ControlToken multi;
    const multiclient::Identity nobody;

    const multiclient::GateResult in_single =
        single.gate(nobody, multiclient::Traffic::user_caused, multiclient::MotionState{});
    CHECK(!in_single.refused);
    CHECK(!single.has_holder());

    const multiclient::GateResult in_multi =
        multi.gate(nobody, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user);
    CHECK(!in_multi.refused);
    CHECK(!multi.has_holder());
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
    // Ties classify_command_line() and gate() together with the actual
    // manual tool-change confirm text, end to end: a tool-change wait
    // (TOOL/WAIT, run=false) never blocks a transfer (see the test above),
    // and this specific command is never on the automatic allow-list (see
    // "classify_command_line: everything else is user-caused"), so any
    // identified client sending it takes control -- the mechanism the
    // design calls "a confirm from any identified client is accepted and
    // moves control through the gate", without a second, parallel notion of
    // control for tool-change waits.
    TEST("gate: a tool-change confirm from a non-holder during a tool-change wait takes control");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{});

    const multiclient::GateResult result =
        token.gate(workshop, classify("M490.2"), multiclient::MotionState{});
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

  {
    TEST("classify_passive_action: suspend/abort/upload are pause/stop/upload");
    CHECK(classify_passive("suspend") == multiclient::PassiveAction::pause);
    CHECK(classify_passive("abort") == multiclient::PassiveAction::stop);
    CHECK(classify_passive("upload test.nc") == multiclient::PassiveAction::upload);
    CHECK(classify_passive("  upload test.nc") == multiclient::PassiveAction::upload);
  }

  {
    TEST("classify_passive_action: a same-prefix word or anything else is none");
    CHECK(classify_passive("suspended") == multiclient::PassiveAction::none);
    CHECK(classify_passive("aborting") == multiclient::PassiveAction::none);
    CHECK(classify_passive("uploads") == multiclient::PassiveAction::none);
    CHECK(classify_passive("download test.nc") == multiclient::PassiveAction::none);
    CHECK(classify_passive("resume") == multiclient::PassiveAction::none);
    CHECK(classify_passive("play test.nc") == multiclient::PassiveAction::none);
    CHECK(classify_passive("") == multiclient::PassiveAction::none);
    CHECK(classify_passive("   ") == multiclient::PassiveAction::none);
  }

  {
    TEST("gate: passing no mode reproduces single-user behaviour exactly (default arguments)");
    // Every call above this point already proves the 3-argument call sites
    // (identical to before this file existed) are unaffected. This checks
    // the same holds when the new mode/action/rights parameters are passed
    // explicitly as their default values.
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::single_user,
               multiclient::PassiveAction::none, multiclient::PassiveRights::watch_only);
    const multiclient::GateResult result =
        token.gate(workshop, multiclient::Traffic::user_caused, multiclient::MotionState{},
                   multiclient::Mode::single_user, multiclient::PassiveAction::none,
                   multiclient::PassiveRights::watch_only);
    CHECK(!result.refused);
    CHECK(result.holder_changed);
    CHECK(token.holder().id == 2);
  }

  {
    TEST("gate: single-user mode ignores action/rights entirely -- a pause still takes control");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{});
    const multiclient::GateResult result =
        token.gate(workshop, multiclient::Traffic::user_caused, multiclient::MotionState{},
                   multiclient::Mode::single_user, multiclient::PassiveAction::pause,
                   multiclient::PassiveRights::watch_only);
    CHECK(!result.refused);
    CHECK(result.holder_changed);
    CHECK(token.holder().id == 2);
  }

  {
    TEST("gate: multi-user, watch_only -- a non-holder write is refused, naming the holder");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user);

    const multiclient::GateResult result =
        token.gate(workshop, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user,
                   multiclient::PassiveAction::none, multiclient::PassiveRights::watch_only);
    CHECK(result.refused);
    CHECK(result.reason == multiclient::RefusalReason::not_holder);
    CHECK(!result.holder_changed);
    CHECK(token.holder().id == 1);
  }

  {
    TEST("gate: multi-user, watch_only -- pause and stop are refused too (level too low)");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user);

    const multiclient::GateResult pause_result =
        token.gate(workshop, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user,
                   multiclient::PassiveAction::pause, multiclient::PassiveRights::watch_only);
    CHECK(pause_result.refused);
    CHECK(pause_result.reason == multiclient::RefusalReason::not_holder);

    const multiclient::GateResult stop_result =
        token.gate(workshop, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user,
                   multiclient::PassiveAction::stop, multiclient::PassiveRights::watch_only);
    CHECK(stop_result.refused);
  }

  {
    TEST("gate: multi-user, watch_stop -- pause/stop execute without moving control");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user);

    const multiclient::GateResult pause_result =
        token.gate(workshop, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user,
                   multiclient::PassiveAction::pause, multiclient::PassiveRights::watch_stop);
    CHECK(!pause_result.refused);
    CHECK(!pause_result.holder_changed);
    CHECK(token.holder().id == 1);  // still Office

    const multiclient::GateResult stop_result =
        token.gate(workshop, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user,
                   multiclient::PassiveAction::stop, multiclient::PassiveRights::watch_stop);
    CHECK(!stop_result.refused);
    CHECK(!stop_result.holder_changed);
    CHECK(token.holder().id == 1);
  }

  {
    TEST("gate: multi-user, watch_stop -- upload is still refused (needs the top level)");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user);

    multiclient::MotionState idle;
    idle.idle = true;
    const multiclient::GateResult result =
        token.gate(workshop, multiclient::Traffic::user_caused, idle, multiclient::Mode::multi_user,
                   multiclient::PassiveAction::upload, multiclient::PassiveRights::watch_stop);
    CHECK(result.refused);
    CHECK(result.reason == multiclient::RefusalReason::not_holder);
  }

  {
    TEST("gate: multi-user, watch_stop_upload -- upload while idle executes without moving control");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user);

    multiclient::MotionState idle;
    idle.idle = true;
    const multiclient::GateResult result =
        token.gate(workshop, multiclient::Traffic::user_caused, idle, multiclient::Mode::multi_user,
                   multiclient::PassiveAction::upload, multiclient::PassiveRights::watch_stop_upload);
    CHECK(!result.refused);
    CHECK(!result.holder_changed);
    CHECK(token.holder().id == 1);
  }

  {
    TEST("gate: multi-user, watch_stop_upload -- upload while NOT idle is refused");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user);

    multiclient::MotionState playing;
    playing.run = true;
    playing.job_playing = true;  // a job is running: not idle
    const multiclient::GateResult result =
        token.gate(workshop, multiclient::Traffic::user_caused, playing, multiclient::Mode::multi_user,
                   multiclient::PassiveAction::upload, multiclient::PassiveRights::watch_stop_upload);
    CHECK(result.refused);
    CHECK(result.reason == multiclient::RefusalReason::not_holder);
  }

  {
    TEST("gate: multi-user, held by another -- a privileged action never moves control even during motion");
    // A stop must be able to interrupt a jog. Unlike the single-user/free-
    // control path, motion in progress plays no part in this decision at
    // all: only who holds control and what the rights level allows do.
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user);

    multiclient::MotionState jogging;
    jogging.run = true;
    const multiclient::GateResult result =
        token.gate(workshop, multiclient::Traffic::user_caused, jogging, multiclient::Mode::multi_user,
                   multiclient::PassiveAction::stop, multiclient::PassiveRights::watch_stop);
    CHECK(!result.refused);
    CHECK(!result.holder_changed);
    CHECK(token.holder().id == 1);
  }

  {
    TEST("gate: multi-user, already the holder -- its own pause/stop is a plain no-op, not a privileged path");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user);

    const multiclient::GateResult result =
        token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user,
                   multiclient::PassiveAction::stop, multiclient::PassiveRights::watch_only);
    CHECK(!result.refused);
    CHECK(!result.holder_changed);
    CHECK(token.holder().id == 1);
  }

  {
    TEST("gate: multi-user, control free -- a write takes control exactly like single-user mode");
    multiclient::ControlToken token;
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    const multiclient::GateResult result =
        token.gate(workshop, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user,
                   multiclient::PassiveAction::none, multiclient::PassiveRights::watch_only);
    CHECK(!result.refused);
    CHECK(result.holder_changed);
    CHECK(token.holder().id == 2);
  }

  {
    TEST("gate: multi-user, control free -- refused (motion_in_progress) while an interactive move is on");
    multiclient::ControlToken token;
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    multiclient::MotionState jogging;
    jogging.run = true;
    const multiclient::GateResult result =
        token.gate(workshop, multiclient::Traffic::user_caused, jogging, multiclient::Mode::multi_user);
    CHECK(result.refused);
    CHECK(result.reason == multiclient::RefusalReason::motion_in_progress);
    CHECK(!token.has_holder());
  }

  {
    TEST("gate: multi-user, automatic traffic never moves control even while someone else holds it");
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user);

    const multiclient::GateResult result =
        token.gate(workshop, multiclient::Traffic::automatic, multiclient::MotionState{}, multiclient::Mode::multi_user);
    CHECK(!result.refused);
    CHECK(!result.holder_changed);
    CHECK(token.holder().id == 1);
  }

  {
    TEST("gate: release then any user-caused message takes control -- 'the next one takes it, from anywhere'");
    // The passive-rights exemption from taking control (tested above) only
    // applies while someone else already holds control -- once it is free,
    // gate() falls through to the same "control free" branch single-user
    // mode always uses, and ANY user-caused message takes control there,
    // whatever it is classified as. That is the literal rule: "the next
    // user-caused message from anywhere takes it".
    multiclient::ControlToken token;
    const multiclient::Identity office = make_identity(1, "Office");
    const multiclient::Identity workshop = make_identity(2, "Workshop");
    token.gate(office, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user);

    CHECK(token.release_if_holder(1));  // office releases
    CHECK(!token.has_holder());

    // Even a "stop" takes control now that nobody holds it -- the privilege
    // that let it execute without moving control (tested above) only
    // matters while someone else is the holder.
    const multiclient::GateResult result =
        token.gate(workshop, multiclient::Traffic::user_caused, multiclient::MotionState{}, multiclient::Mode::multi_user,
                   multiclient::PassiveAction::stop, multiclient::PassiveRights::watch_stop);
    CHECK(!result.refused);
    CHECK(result.holder_changed);
    CHECK(token.holder().id == 2);
  }

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
