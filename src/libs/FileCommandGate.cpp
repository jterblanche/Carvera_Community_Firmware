#include "FileCommandGate.h"

namespace file_command_gate {

Decision decide(FileCommand command, const MachineState& state) {
  // Checked first, so a transfer refused during a job names the job.
  if (state.job_playing) return Decision::refuse_job_playing;
  if (command == FileCommand::upload && state.job_start_pending) return Decision::refuse_job_start_pending;
  const bool transfer = command == FileCommand::download || command == FileCommand::upload;
  if (transfer && !state.motion_queue_idle) return Decision::refuse_machine_busy;
  return Decision::run;
}

}  // namespace file_command_gate
