#include <cstdio>
#include <cstring>

#include "libs/FileCommandGate.h"

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

using file_command_gate::Decision;
using file_command_gate::FileCommand;
using file_command_gate::MachineState;
using file_command_gate::decide;

const FileCommand all_commands[] = {
    FileCommand::download,
    FileCommand::upload,
    FileCommand::md5sum,
    FileCommand::cat,
    FileCommand::md5check,
};

MachineState state(bool job_playing, bool motion_queue_idle) {
  MachineState s;
  s.job_playing = job_playing;
  s.motion_queue_idle = motion_queue_idle;
  return s;
}

bool is_transfer(FileCommand command) { return command == FileCommand::download || command == FileCommand::upload; }

}  // namespace

int main() {
  {
    TEST("decide: no job and an empty motion queue: every command runs");
    for (FileCommand command : all_commands) CHECK(decide(command, state(false, true)) == Decision::run);
  }

  {
    TEST("decide: a default MachineState (no job, empty queue) runs every command");
    for (FileCommand command : all_commands) CHECK(decide(command, MachineState{}) == Decision::run);
  }

  {
    TEST("decide: no job, moves queued: download and upload get the machine-busy reply, the rest run");
    for (FileCommand command : all_commands) {
      const Decision expected = is_transfer(command) ? Decision::refuse_machine_busy : Decision::run;
      CHECK(decide(command, state(false, false)) == expected);
    }
  }

  {
    TEST("decide: a job playing with the motion queue empty between moves: every command is refused");
    for (FileCommand command : all_commands) {
      CHECK(decide(command, state(true, true)) == Decision::refuse_job_playing);
    }
  }

  {
    TEST("decide: a job playing with moves queued: every command is refused for the job, not as busy");
    for (FileCommand command : all_commands) {
      CHECK(decide(command, state(true, false)) == Decision::refuse_job_playing);
    }
  }

  {
    TEST("job_playing_reply: one line in the control gate's refusal form");
    const char* reply = file_command_gate::job_playing_reply;
    const std::size_t length = std::strlen(reply);
    const char prefix[] = "error:Refused -- ";
    CHECK(std::strncmp(reply, prefix, sizeof(prefix) - 1) == 0);
    CHECK(length > sizeof(prefix) + 1);
    CHECK(std::strcmp(reply + length - 2, "\r\n") == 0);
    CHECK(std::strpbrk(reply, "\r\n") == reply + length - 2);
    CHECK(std::strstr(reply, "job") != nullptr);
  }

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
