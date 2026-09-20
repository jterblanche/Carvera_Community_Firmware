/*
      This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl).
      Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
      Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
      You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef STREAMOUTPUTPOOL_H
#define STREAMOUTPUTPOOL_H

using namespace std;
#include <set>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdarg>

#include "libs/StreamOutput.h"

class StreamOutputPool : public StreamOutput {

public:
    StreamOutputPool(){
    }

    int puts(const char* s, int size)
    {
        // Calling through the pool (THEKERNEL->streams->printf/puts) means
        // every registered stream, by definition -- unlike a specific
        // gcode's own stream, which targets one link. A multi-client stream
        // (WifiProvider) uses this flag to broadcast to every client of
        // its own, rather than whichever one it happens to be mid-dispatch
        // for when the pool call arrives (see WifiProvider::puts()); a
        // single-client stream (SerialConsole) has no use for it.
        const bool was_broadcasting = broadcasting;
        broadcasting = true;
        int r = 0;
        for(set<StreamOutput*>::iterator i = this->streams.begin(); i != this->streams.end(); i++)
        {
            int k;
            if (communication_protocol == PROTOCOL_SMOOTHIE) {
                k = (*i)->puts(s);
            }
            else {
                k = (*i)->puts(s,size);
            }
            if (k > r)
                r = k;
        }
        broadcasting = was_broadcasting;
        return r;
    }

    // True for the duration of the loop above. Save/restore, not a plain
    // reset to false, in case a stream's puts() somehow re-enters the pool.
    static bool is_broadcasting() { return broadcasting; }

    // Reaches every registered stream's own publish_multiclient() override,
    // so a fact discovered in one place (Player.cpp, a halt handler) reaches
    // every transport's identified clients, not just whichever one caused
    // it. See StreamOutput::publish_multiclient().
    void publish_multiclient(char cmd, const uint8_t* payload, size_t length)
    {
        for(set<StreamOutput*>::iterator i = this->streams.begin(); i != this->streams.end(); i++)
        {
            (*i)->publish_multiclient(cmd, payload, length);
        }
    }

    void append_stream(StreamOutput* stream)
    {
        this->streams.insert(stream);
    }

    void remove_stream(StreamOutput* stream)
    {
        this->streams.erase(stream);
    }

    bool frames_protocol_output() const { return true; }

    void on_protocol_changed()
    {
        for(set<StreamOutput*>::iterator i = this->streams.begin(); i != this->streams.end(); i++) {
            (*i)->on_protocol_changed();
        }
    }

private:
    set<StreamOutput*> streams;
    inline static bool broadcasting = false;
};

#endif
