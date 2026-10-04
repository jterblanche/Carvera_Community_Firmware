#pragma once

// Whether the machine's current state refuses a command that writes machine
// settings: config-set, config-delete, config-load, config-restore and
// config-default. Reading settings (config-get, config-get-all) is never
// gated and does not go through this. Plain C++, no Kernel or mbed
// dependency, so it builds and runs on the host (see
// tests/TEST_ConfigWriteGate) the same way ControlToken does.
namespace config_write_gate {

// The caller's own reduction of Kernel::get_state() (libs/Kernel.h) to the
// three states that still accept a settings write -- so a setting that
// caused an alarm can still be corrected, and the machine can still be put
// to sleep or woken without carrying settings changes through this check.
// Every other state -- a job running or paused, homing, a jog, probe or
// move in progress, or a tool change under way -- refuses the write.
struct MachineState {
  bool idle = false;
  bool alarm = false;
  bool sleeping = false;
};

// True if a settings write should be refused right now.
bool refuses_write(const MachineState& state);

}  // namespace config_write_gate
