/*
      This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl).
      Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
      Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
      You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef STREAMOUTPUT_H
#define STREAMOUTPUT_H

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdio.h>

// This is a base class for all StreamOutput objects.
// StreamOutputs are basically "things you can sent strings to". They are passed along with gcodes for example so modules can answer to those gcodes.
// They are usually associated with a command source, but can also be a NullStreamOutput if we just want to ignore whatever is sent

class NullStreamOutput;

enum ProtocolMode {
    PROTOCOL_SMOOTHIE = 0,
    PROTOCOL_MAKERA   = 1
};

extern ProtocolMode communication_protocol;

class StreamOutput {
    public:
        StreamOutput(){}
        virtual ~StreamOutput(){}

        virtual int printf(const char *format, ...) __attribute__ ((format(printf, 2, 3)));
        virtual int _putc(int c) { return 1; }
        virtual int _getc(void) { return 0; }
        virtual int gets(char** buf, int size = 0) { return 0; }
        virtual int puts(const char* buf, int size = 0) = 0;
        virtual bool ready() { return true; };
        virtual int type() {return 0; }; // 0: serial, 1: wifi
        virtual void reset(void) {return ; };
        // False once the client that started the current file transfer has
        // disconnected, so the transfer can give up at once. A stream with a
        // single fixed link (serial, USB) has no such client and never says so.
        virtual bool transfer_client_connected() { return true; }
        virtual bool frames_protocol_output() const { return false; }
        virtual void on_protocol_changed() {}
        virtual int printfcmd(const char cmd, const char *format, ...) __attribute__ ((format(printf, 3, 4)));

        // Publishes `payload` (a frame of type `cmd`, already encoded --
        // see libs/Publish.h) to this stream's own identified clients, if
        // it has any. Called through THEKERNEL->streams so it reaches every
        // transport at once (libs/StreamOutputPool.h); a stream that has no
        // notion of "identified clients" (a log file, the pool itself)
        // leaves the default no-op.
        virtual void publish_multiclient(char cmd, const uint8_t *payload, std::size_t length) { (void)cmd; (void)payload; (void)length; }

        // Repeats `payload`, opaque and untouched, to this stream's own
        // identified clients other than `source_id` (libs/Publish.h's
        // build_relay_frame() adds the source id prefix and does the size
        // check; this only decides who receives it). Reached through
        // THEKERNEL->streams so every transport's own clients see it (see
        // StreamOutputPool::publish_relay()), the same shape as
        // publish_multiclient() above; a stream with no notion of
        // "identified clients" leaves the default no-op.
        virtual void publish_relay(uint64_t source_id, const uint8_t *payload, std::size_t length) {
            (void)source_id; (void)payload; (void)length;
        }

        static NullStreamOutput NullStream;

        // Virtual because printf()/printfcmd() above call it to frame their
        // output, and a transport that needs to do something more with that
        // frame -- publishing a copy to the other connected clients -- can
        // only get the chance if the call dispatches to its own version.
        // Left non-virtual, a derived PacketMessage silently shadows this
        // one instead of overriding it, and every reply sent through
        // printf() runs this base version and nothing else.
        virtual void PacketMessage(char cmd, const char* s, int size);
};

class NullStreamOutput : public StreamOutput {
    public:
        int printf(const char *format, ...) { return 0; }
        int puts(const char* str, int size = 0) { return strlen(str); }
};

#endif
