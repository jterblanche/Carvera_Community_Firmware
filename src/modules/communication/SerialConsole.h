/*
      This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl).
      Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
      Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
      You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef SERIALCONSOLE_H
#define SERIALCONSOLE_H

#include "libs/Module.h"
#include "Serial.h" // mbed.h lib
#include "libs/Kernel.h"
#include <vector>
#include <string>
using std::string;
#include "libs/RingBuffer.h"
#include "libs/StreamOutput.h"
#include "libs/MakeraFrame.h"
#include "libs/ControlToken.h"
#include "libs/Hello.h"
#include "libs/Publish.h"


#define baud_rate_setting_checksum CHECKSUM("baud_rate")

class SerialConsole : public Module, public StreamOutput {
    public:
        SerialConsole( PinName tx_pin, PinName rx_pin, int baud_rate );
        ~SerialConsole();

        void on_module_loaded();
        void on_serial_char_received();
        void on_main_loop(void * argument);
        void on_idle(void * argument);
        void on_second_tick(void * argument);
        void on_set_public_data(void *argument);
        bool has_char(char letter);
        void set_rx_enabled(bool enabled);

        int _putc(int c);
        int _getc(void);
        int puts(const char*, int size = 0);
        int gets(char** buf, int size = 0);
        bool ready();
        bool frames_protocol_output() const { return true; }
        void on_protocol_changed();
        void PacketMessage(char cmd, const char* s, int size) override;
        void publish_multiclient(char cmd, const uint8_t* payload, size_t length);
        void publish_relay(uint64_t source_id, const uint8_t* payload, size_t length);
        void reset();
        char getc_result;

        int get_baud() const { return current_baud_rate; }
        void set_baud_temporary(int new_baud);
        int receive_packet(makera::Packet& packet, uint32_t timeout_ms = 100);

        //string receive_buffer;                 // Received chars are stored here until a newline character is received
        //vector<std::string> received_lines;    // Received lines are stored here until they are requested
        RingBuffer<char,256> buffer;             // Receive buffer
        mbed::Serial* serial;
        char previous_char;                       // Track previous character for ?1 detection
        int current_baud_rate;
        int default_baud_rate;
        int temp_baud_rate;                       // non-zero = temporary baud active
        uint32_t last_activity_us;                // for 15s timeout revert (a raw us_ticker_read() reading, not a value divided down to ms)
        makera::FrameDecoder makera_frame_decoder;
#if defined(SERIAL_RX_DMA)
        bool rx_dispatch_enabled;
        int rx_lookahead;
        bool handle_rx_error();
#endif
        int read_byte();

        enum FileParseState {
            FILE_WAIT_HEADER,
            FILE_READ_LENGTH,
            FILE_READ_DATA,
            FILE_CHECK_FOOTER
        } file_parse_state;
        uint16_t file_frame_index;
        uint16_t file_bytes_needed;
        uint8_t file_header[2];
        uint8_t file_footer[2];

        void process_makera_byte(uint8_t received);
        void reset_file_parser();
        int check_file_packet(char **buf);
        void handle_hello(const uint8_t* payload, uint16_t payload_length, uint32_t now_us);
        void handle_client_list_request();
        // The multi_client.mode byte a hello ack reports. See
        // WifiProvider::hello_ack_mode() for the same on the WiFi side.
        uint8_t hello_ack_mode() const;
        // Hands a decoded relay frame to publish_relay(), if this USB link
        // is identified. See WifiProvider::handle_wifi_relay() for the same
        // decision on the WiFi side.
        void handle_relay(const uint8_t* payload, uint16_t payload_length);
        // Frees control if this USB link currently holds it, and publishes
        // a control-changed event naming nobody if it did. See
        // WifiProvider::handle_wifi_control_release() for the same on the
        // WiFi side. Multi-user mode only -- the caller does not even call
        // this in single-user mode (see process_makera_byte()).
        void handle_control_release();

        // Publishes `text` (a command's own text, or its reply) as one or
        // more published-console-line fragments, tagged with this USB
        // link's own id/name, to every identified client across every
        // transport. See publish_multiclient() and WifiProvider's own
        // equivalent.
        void publish_console_line(const char* text, size_t length);

        // The control-token gate (libs/ControlToken.h), for the one USB
        // command about to be dispatched. See
        // WifiProvider::gate_dispatch() for the same gate on the WiFi
        // link, against the same shared ControlToken -- one rule, two thin
        // per-transport call sites, since each transport addresses its own
        // refusal reply differently.
        bool gate_dispatch(const makera::Packet& packet);

        // multi_client.status_publish_hz, converted once at load time.
        uint32_t status_publish_interval_us;
        uint32_t last_status_publish_us = 0;

        // multi_client.mode and multi_client.passive_rights, read once at
        // load time. gate_dispatch() reads these every call rather than
        // caching a decision, since they never change at runtime.
        multiclient::Mode multi_client_mode;
        multiclient::PassiveRights multi_client_passive_rights;
        struct {
          volatile bool query_flag:1;
          volatile bool halt_flag:1;
          volatile bool diagnose_flag:1;
          bool command_waiting:1;
        };
        volatile bool makera_file_cancel;
        volatile bool makera_rx_overflow;
};

#endif
