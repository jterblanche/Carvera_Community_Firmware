/*
      This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl).
      Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
      Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
      You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/


#ifndef configurator_h
#define configurator_h

#include <string>
using std::string;

class StreamOutput;

// Whether a settings write should be refused right now because the machine
// is busy (see libs/ConfigWriteGate.h): Idle, Alarm and Sleep accept it,
// same as always; every other state -- a job running or paused, homing, a
// jog, probe or move in progress, or a tool change under way -- refuses it.
// Shared by Configurator's own write commands below and by SimpleShell's
// config_restore_command/config_default_command, which are not part of this
// class but write settings the same way.
bool configurator_refuses_write();

class Configurator
{
public:
    Configurator() {}

    void config_get_command( string parameters, StreamOutput *stream );
    void config_set_command( string parameters, StreamOutput *stream );
    void config_delete_command( string parameters, StreamOutput *stream );
    void config_load_command(string parameters, StreamOutput *stream );
};


#endif // configurator_h
