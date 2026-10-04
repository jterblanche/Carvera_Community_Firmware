#include "ConfigWriteGate.h"

namespace config_write_gate {

bool refuses_write(const MachineState& state) { return !(state.idle || state.alarm || state.sleeping); }

}  // namespace config_write_gate
