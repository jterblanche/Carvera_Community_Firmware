#pragma once

#include <cstdint>

// Whether a file command may run right now. Each of these commands runs to
// completion inside the command itself: a file transfer, or reading a whole
// file. Until it returns, the main loop does not run, so the player reads no
// new lines from a playing job, and the machine stops once the moves it has
// already planned run out. Plain C++, no Kernel or mbed dependency, so it
// builds and runs on the host (see tests/TEST_FileCommandGate) the same way
// ConfigWriteGate does.
namespace file_command_gate {

// The reply to a command refused because a job is playing or paused. It uses
// the same "error:Refused -- " form as the control gate's own refusals, which
// controllers already recognise as a refusal.
constexpr char job_playing_reply[] = "error:Refused -- a job is playing\r\n";

enum class FileCommand : uint8_t {
  download,  // download: send a file to the controller
  upload,    // upload: receive a file from the controller
  md5sum,    // md5sum: hash a whole file
  cat,       // cat: print a whole file
  md5check,  // M576, M576.1, M576.2: hash files and compare them to their sidecars
};

// The caller's snapshot of the machine, taken when the command arrives.
struct MachineState {
  // A file is playing, or playing and paused (Player's playing_file).
  bool job_playing = false;
  // The motion queue is empty (Conveyor::is_idle()).
  bool motion_queue_idle = true;
  // A job start is held, waiting for other controllers to load the file
  // (libs/JobStartWait.h).
  bool job_start_pending = false;
};

enum class Decision : uint8_t {
  run,
  // Refused because a job is playing or paused: reply with job_playing_reply.
  refuse_job_playing,
  // download or upload with moves still in the queue: the machine-busy reply
  // those two commands have always sent.
  refuse_machine_busy,
  // upload while a job start is held: reply with
  // multiclient::job_start_pending_reply. An upload could replace the file
  // the other controllers are loading. The other file commands only read,
  // and a waiting controller needs download to fetch the job, so they run.
  refuse_job_start_pending,
};

Decision decide(FileCommand command, const MachineState& state);

}  // namespace file_command_gate
