/*
      This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl).
      Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
      Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
      You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/

#include <string>
#include <string.h>
#include <stdarg.h>
using std::string;
#include "mbed.h" // for us_ticker_read()
#include "libs/Module.h"
#include "libs/Kernel.h"
#include "libs/CRC16.h"
#include "libs/MakeraControl.h"
#include "libs/MakeraFrame.h"
#include "libs/ClientTable.h"
#include "libs/JobStartWait.h"
#include "libs/IdentityCheck.h"
#include "libs/nuts_bolts.h"
#include "SerialConsole.h"
#include "libs/RingBuffer.h"
#include "libs/SerialMessage.h"
#include "libs/StreamOutput.h"
#include "libs/StreamOutputPool.h"
#include "ATCHandlerPublicAccess.h"
#include "modules/utils/player/PlayerPublicAccess.h"
#include "PublicDataRequest.h"
#include "PublicData.h"
#include "libs/Config.h"
#include "checksumm.h"
#include "ConfigValue.h"
#if defined(SERIAL_RX_DMA)
#include "UartRxDma.h"
#endif
#if defined(MACHINE_FAMILY_Z1)
#include "version.h"
#endif

#define uart_checksum CHECKSUM("uart")
#define XBUFF_LENGTH 8208

// Governs both WiFi and USB, so it lives in its own namespace rather than
// under "uart.".
// Same checksum values as WifiProvider.cpp's own copy of these two defines
// (CHECKSUM() hashes the string, not the symbol).
#define multi_client_checksum      CHECKSUM("multi_client")
#define status_publish_hz_checksum CHECKSUM("status_publish_hz")
#define multi_client_mode_checksum CHECKSUM("mode")
#define passive_rights_checksum    CHECKSUM("passive_rights")

extern unsigned char xbuff[XBUFF_LENGTH];

static makera::Packet makera_packet;
static RingBuffer<char, 1024> makera_rx_bytes;
// Let a back-to-back burst finish before command handlers reply on the same UART.
constexpr uint32_t makera_rx_quiet_us = 2000;
constexpr int uart_rx_error = -2;
#if defined(MACHINE_FAMILY_Z1)
static uint32_t last_version_us;
constexpr uint32_t version_interval_us = 5 * 1000 * 1000;
#endif

// Serial reading module
// Treats every received line as a command and passes it ( via event call ) to the command dispatcher.
// The command dispatcher will then ask other modules if they can do something with it
SerialConsole::SerialConsole( PinName tx_pin, PinName rx_pin, int baud_rate )
    : makera_frame_decoder(makera_packet) {
    this->serial = new mbed::Serial( tx_pin, rx_pin );
    this->serial->baud(baud_rate);
    this->previous_char = 0;
    this->current_baud_rate = baud_rate;
    this->default_baud_rate = baud_rate;
    this->temp_baud_rate = 0;
    this->last_activity_us = 0;
    this->status_publish_interval_us = multiclient::status_publish_interval_us(multiclient::default_status_publish_hz);
    this->multi_client_mode = multiclient::Mode::single_user;
    this->multi_client_passive_rights = multiclient::PassiveRights::watch_stop_upload;
    this->makera_rx_overflow = false;
    this->command_waiting = false;
    this->answering_automatic = false;
    this->makera_frame_decoder.reset();
    makera_rx_bytes.tail = makera_rx_bytes.head;
    this->reset_file_parser();
#if defined(SERIAL_RX_DMA)
    this->rx_dispatch_enabled = false;
    this->rx_lookahead = -1;
    this->serial->attach(nullptr, mbed::Serial::RxIrq);
    uart2_rx_dma::initialize();
#endif
}

SerialConsole::~SerialConsole(){
    delete this->serial;
}

// Called when the module has just been loaded
void SerialConsole::on_module_loaded() {
    // We want to be called every time a new char is received
    query_flag = false;
    halt_flag = false;
    diagnose_flag = false;
    makera_file_cancel = false;

    // multi_client., not uart. -- this also governs WiFi's own proactive
    // status publish (WifiProvider).
    int configured_publish_hz =
        THEKERNEL->config->value(multi_client_checksum, status_publish_hz_checksum)->as_int(multiclient::default_status_publish_hz);
    if (configured_publish_hz < 0) configured_publish_hz = multiclient::default_status_publish_hz;
    this->status_publish_interval_us = multiclient::status_publish_interval_us(static_cast<uint16_t>(configured_publish_hz));

    // multi_client.mode: single-user unless the config explicitly says
    // "multi_user" -- an unrecognised value is treated the same as absent,
    // so a typo cannot silently turn multi-user mode on. Same config key
    // WifiProvider.cpp reads, so both links agree on the mode.
    std::string configured_mode = THEKERNEL->config->value(multi_client_checksum, multi_client_mode_checksum)->as_string(multiclient::mode_single_user);
    if (configured_mode == multiclient::mode_multi_user) {
        this->multi_client_mode = multiclient::Mode::multi_user;
    } else {
        if (configured_mode != multiclient::mode_single_user) {
            THEKERNEL->streams->printf("USB: multi_client.mode '%s' not recognised, using single_user\n", configured_mode.c_str());
            THEKERNEL->set_config_load_error(true);
        }
        this->multi_client_mode = multiclient::Mode::single_user;
    }

    // multi_client.passive_rights: how much a non-holder may do in
    // multi-user mode. watch_stop_upload (the highest level) is the
    // default when the setting is absent; an unrecognised value falls back
    // to the lowest level instead, so a typo narrows rights rather than
    // widening them -- and is recorded as a config load error, so the
    // machine says so rather than quietly running at a level nobody
    // asked for. Every value here is short enough to survive
    // CONFIGVALUE_MAX_LEN; a longer one would be truncated on read and
    // would land in this branch.
    std::string configured_rights =
        THEKERNEL->config->value(multi_client_checksum, passive_rights_checksum)->as_string(multiclient::rights_watch_stop_upload);
    if (configured_rights == multiclient::rights_watch_stop_upload) {
        this->multi_client_passive_rights = multiclient::PassiveRights::watch_stop_upload;
    } else if (configured_rights == multiclient::rights_watch_stop) {
        this->multi_client_passive_rights = multiclient::PassiveRights::watch_stop;
    } else if (configured_rights == multiclient::rights_watch_only) {
        this->multi_client_passive_rights = multiclient::PassiveRights::watch_only;
    } else {
        THEKERNEL->streams->printf("USB: multi_client.passive_rights '%s' not recognised, using watch_only\n", configured_rights.c_str());
        THEKERNEL->set_config_load_error(true);
        this->multi_client_passive_rights = multiclient::PassiveRights::watch_only;
    }

#if defined(MACHINE_FAMILY_CARVERA)
    default_baud_rate = THEKERNEL->config->value(uart_checksum, baud_rate_setting_checksum)->as_number(current_baud_rate);
    if (default_baud_rate != current_baud_rate) {
        this->serial->baud(default_baud_rate);
        this->current_baud_rate = default_baud_rate;
    }
#endif

    this->set_rx_enabled(true);

    // USB's own entry in the shared client table (see ClientTable.h), for
    // the 3 WiFi + 1 USB cap. The firmware cannot detect a USB host (the
    // chip's native USB is unused; this link is a UART), so unlike a WiFi
    // socket there is no connect/disconnect event to key this on -- it is
    // simply always present from boot.
    multiclient::shared_client_table().set_usb_present(true, us_ticker_read());

    // We only call the command dispatcher in the main loop, nowhere else
    this->register_for_event(ON_MAIN_LOOP);
    this->register_for_event(ON_IDLE);
    this->register_for_event(ON_SET_PUBLIC_DATA);
    // Reconciles the control token once a second (see on_second_tick()) --
    // registered here, not on WifiProvider, because SerialConsole is the
    // one module guaranteed to exist on every build (WifiProvider is
    // compiled out entirely under NO_WIFI_PROVIDER, the z1 target).
    this->register_for_event(ON_SECOND_TICK);

    // Add to the pack of streams kernel can call to, for example for broadcasting
    THEKERNEL->streams->append_stream(this);
}

void SerialConsole::set_baud_temporary(int new_baud) {
    this->temp_baud_rate = new_baud;
    this->current_baud_rate = new_baud;
    this->serial->baud(new_baud);
    this->last_activity_us = us_ticker_read();
}

void SerialConsole::set_rx_enabled(bool enabled) {
#if defined(SERIAL_RX_DMA)
    this->rx_dispatch_enabled = enabled;
#else
	if (enabled) {
	    this->serial->attach(this, &SerialConsole::on_serial_char_received, mbed::Serial::RxIrq);
	} else {
	    this->serial->attach(nullptr, mbed::Serial::RxIrq);
	}
#endif
}

void SerialConsole::on_set_public_data(void *argument) {
    PublicDataRequest* pdr = static_cast<PublicDataRequest*>(argument);

    if(!pdr->starts_with(atc_handler_checksum)) return;

    if(pdr->second_element_is(set_serial_rx_irq_checksum)) {
        bool enable_irq = *static_cast<bool *>(pdr->get_data_ptr());
        this->set_rx_enabled(enable_irq);
        pdr->set_taken();
    }
}


// Drain bytes supplied by the active IRQ- or DMA-backed transport.
void SerialConsole::on_serial_char_received() {
	int received_byte;
	while ((received_byte = this->read_byte()) >= 0) {
		char received = static_cast<char>(received_byte);
		last_activity_us = us_ticker_read();

		if(THEKERNEL->is_cachewait()) {
			continue;
		}

        if (communication_protocol == PROTOCOL_MAKERA) {
            // The firmware cannot detect a bare USB cable, only bytes
            // actually arriving on it -- this is where a USB controller
            // starts counting as present for the old-client rule.
            multiclient::shared_client_table().start_usb_hello_window(last_activity_us);
            const int next = makera_rx_bytes.next_block_index(makera_rx_bytes.head);
            if (next == makera_rx_bytes.tail) {
                makera_rx_overflow = true;
            } else {
                makera_rx_bytes.push_back(received);
            }
            continue;
        }
		
		if (received == '?') {
			query_flag = true;
			continue;
		} else if (this->previous_char == '?' && received == '1') {
			// Found ?1 pattern
			query_flag = true;
			THEKERNEL->set_keep_alive_request(true);
			continue;
		}
		
		//if (received == '*') {
		//	diagnose_flag = true;
		//	continue;
		//}
		if (received == 'X'-'A'+1) { // ^X
			halt_flag = true;
			continue;
		}
        if(received == 'Y' - 'A' + 1) { // ^Y
            if(THEKERNEL->get_internal_stop_request()) {
                THEKERNEL->set_internal_stop_request(false);
            } else {
                THEKERNEL->set_stop_request(true); // generic stop what you are doing request
                THEKERNEL->set_stop_request_time(us_ticker_read());
            }
            continue;
        }
        if(received == 'Z' - 'A' + 1) { // ^Z
            THEKERNEL->set_keep_alive_request(true);
            continue;
        }
        if(THEKERNEL->is_feed_hold_enabled()) {
            bool at_line_start = (this->buffer.head == this->buffer.tail) || (this->previous_char == '\n') || (this->previous_char == '\r');
            if(at_line_start) {
                if(received == '!') { // safe pause
                    THEKERNEL->set_feed_hold(true);
                    continue;
                }
                if(received == '~') { // safe resume
                    THEKERNEL->set_feed_hold(false);
                    continue;
                }
            }
        }
		// convert CR to NL (for host OSs that don't send NL)
		if ( received == '\r' ) { received = '\n'; }
		this->buffer.push_back(received);

        // Reset previous_char for any other character
		this->previous_char = received;
    }
}

void SerialConsole::on_idle(void * argument)
{
#if defined(SERIAL_RX_DMA)
    if (!rx_dispatch_enabled) handle_rx_error();
#endif
	if (THEKERNEL->is_uploading()) return;

#if defined(SERIAL_RX_DMA)
    if (rx_dispatch_enabled) on_serial_char_received();
#endif

    const uint32_t now_us = us_ticker_read();
    if (communication_protocol == PROTOCOL_MAKERA && !command_waiting &&
        now_us - last_activity_us >= makera_rx_quiet_us) {
        while (!command_waiting && makera_rx_bytes.tail != makera_rx_bytes.head) {
            char received;
            makera_rx_bytes.pop_front(received);
            process_makera_byte(static_cast<uint8_t>(received));
            if (THEKERNEL->is_uploading()) break;
        }
    }

    // A USB entry whose hello window has started but has gone quiet for
    // usb_idle_timeout_us is treated as no longer present: its identity and
    // hello-window progress are cleared, the same reset a protocol switch
    // already does, so it stops counting toward "present" (and, if it never
    // identified, toward "old and not alone") until something arrives on it
    // again. Without this, a single USB session anywhere in a power cycle
    // would count forever, since nothing else ever un-counts it.
    if (communication_protocol == PROTOCOL_MAKERA) {
        auto &table = multiclient::shared_client_table();
        const multiclient::Client *usb = table.usb();
        if (usb != nullptr && multiclient::usb_session_expired(usb->hello_window_started, now_us, last_activity_us)) {
            // Read id/name before clearing: clear_usb_identity() wipes them.
            // Only an identified session is announced -- one that never got
            // past its hello window had no id or name to announce either.
            if (usb->identified) {
                uint8_t left_payload[1 + 8 + 1 + multiclient::max_name_length];
                const std::size_t left_length = multiclient::build_client_left_event(
                    usb->id, usb->name, usb->name_len, left_payload, sizeof(left_payload));
                if (left_length != 0) THEKERNEL->streams->publish_multiclient(PTYPE_EVENT, left_payload, left_length);
            }
            table.clear_usb_identity();
        }
    }

    if (communication_protocol == PROTOCOL_MAKERA) drive_identity_check(now_us);

#if defined(MACHINE_FAMILY_CARVERA)
    if (temp_baud_rate != 0) {
        if ((now_us - last_activity_us) >= 15000000u) {
            this->serial->baud(default_baud_rate);
            this->current_baud_rate = default_baud_rate;
            this->temp_baud_rate = 0;
        }
    }
#endif

    if (makera_rx_overflow) {
        makera_rx_overflow = false;
        // Bypasses PacketMessage()'s own publish hook via the explicit
        // base-class call: this is a transport-raised notice, not a
        // command's reply, so it should not be tagged and published as one.
        StreamOutput::PacketMessage(PTYPE_NORMAL_INFO, "ERROR: serial receive buffer full\r\n", 0);
    }

    if (makera_file_cancel) {
        makera_file_cancel = false;
        static const char cancel_payload[] = "ok\r\n";
        PacketMessage(PTYPE_FILE_CAN, cancel_payload, sizeof(cancel_payload));
    }

    if (communication_protocol == PROTOCOL_MAKERA &&
        multiclient::publish_due(now_us, last_status_publish_us, status_publish_interval_us)) {
        last_status_publish_us = now_us;
        const multiclient::Client *usb = multiclient::shared_client_table().usb();
        if (usb != nullptr && usb->identified) {
            PacketMessage(PTYPE_STATUS_RES, THEKERNEL->get_query_string().c_str(), 0);
        }
    }

    if (query_flag ) {
        query_flag = false;
        if (communication_protocol == PROTOCOL_SMOOTHIE) {
            puts(THEKERNEL->get_query_string().c_str(), 0);
        } else if (multiclient::must_identify_first(multiclient::shared_client_table(), multiclient::shared_client_table().usb())) {
            // No machine state, but a valid frame (libs/Hello.h).
            PacketMessage(PTYPE_STATUS_RES, "", 0);
        } else {
            PacketMessage(PTYPE_STATUS_RES, THEKERNEL->get_query_string().c_str(), 0);
        }
    }

    if (diagnose_flag) {
    	diagnose_flag = false;
        if (communication_protocol == PROTOCOL_SMOOTHIE) {
			puts(THEKERNEL->get_diagnose_string().c_str(), 0);
        } else {
            PacketMessage(PTYPE_DIAG_RES, THEKERNEL->get_diagnose_string().c_str(), 0);
        }
    }

    if (halt_flag) {
        halt_flag= false;
        THEKERNEL->set_halt_reason(MANUAL);
        
        if (communication_protocol == PROTOCOL_MAKERA) {
            // Bypasses the publish hook the same way the overflow notice
            // above does, and for the same reason -- this is reported
            // separately as the alarm/halt event (Player::on_halt()), not
            // as a published console line.
            StreamOutput::PacketMessage(PTYPE_NORMAL_INFO, "ERROR: Abort during cycle\r\n", 0);
        } else if(THEKERNEL->is_grbl_mode()) {
            puts("ERROR: Abort during cycle\r\n", 0);
        } else {
            puts("ERROR: Abort during cycle\r\nM999 or $X to exit HALT state\r\n", 0);
        }
        THEKERNEL->call_event(ON_HALT, nullptr);
    }

#if defined(MACHINE_FAMILY_Z1)
    // Named apart from the now_us read at the top of this function: both are
    // at function scope, and this block wants its own reading taken here.
    const uint32_t version_now_us = us_ticker_read();
    if (version_now_us - last_version_us > version_interval_us) {
        Version version;
        PacketMessage(PTYPE_FIRM_VER, version.get_build(), 0);
        last_version_us = version_now_us;
    }
#endif
}

// Frees the control token if its holder has disconnected or silently
// dropped (libs/ControlToken.h, reconcile_holder()) -- registered here,
// once a second, rather than hooked into every place either link removes a
// client from the shared table, so the "was that the holder?" decision
// stays in exactly one place regardless of which link, or which of several
// removal paths, actually caused it.
void SerialConsole::on_second_tick(void *argument) {
    if (multiclient::reconcile_holder(multiclient::shared_control_token(), multiclient::shared_client_table())) {
        uint8_t payload[1 + 8 + 1];  // holder_id 0 + holder_name_len 0: nobody has control
        const std::size_t length = multiclient::build_control_changed_event(0, nullptr, 0, payload, sizeof(payload));
        if (length != 0) THEKERNEL->streams->publish_multiclient(PTYPE_EVENT, payload, length);
    }
}

// Builds the inputs ControlToken::gate()/peek() need from `packet`, the same
// way WifiProvider::gate_inputs_for() does for the WiFi link -- shared by
// gate_dispatch() (the authoritative, dispatch-time call) and
// refused_on_arrival() (the early, non-committing call at arrival). The
// motion snapshot is read fresh on every call: see WifiProvider::
// gate_inputs_for()'s own comment for why that is deliberate.
SerialConsole::GateInputs SerialConsole::gate_inputs_for(const makera::Packet &packet) {
    GateInputs in;
    in.traffic = packet.type == PTYPE_FILE_START
        ? multiclient::classify_file_transfer_start()
        : multiclient::classify_command_line(reinterpret_cast<const char*>(packet.data), packet.data_length);
    // Only meaningful in multi-user mode (see ControlToken::gate()), but
    // classified unconditionally -- it is cheap, and gate() ignores it
    // outside multi-user mode.
    in.action = multiclient::classify_passive_action(reinterpret_cast<const char*>(packet.data), packet.data_length);
    in.sender = multiclient::identity_of(multiclient::shared_client_table().usb());

    // See WifiProvider::gate_inputs_for()'s own comment: RUN alone is
    // ambiguous between a running job, a jog, an MDI move and an automatic
    // tool-change move; is_playing() disambiguates it.
    const uint8_t machine_state = THEKERNEL->get_state();
    in.motion.run = (machine_state == RUN);
    in.motion.homing = (machine_state == HOME);
    in.motion.idle = (machine_state == IDLE);
    in.motion.job_playing = player_is_playing();
    return in;
}

// The printf() refusal reply, worded the same way regardless of whether
// gate_dispatch() or refused_on_arrival() is the one sending it. printf(),
// which frames through PacketMessage(), not puts() -- same reasoning as
// WifiProvider::reply_gate_refusal().
void SerialConsole::reply_gate_refusal(const multiclient::GateResult &result) {
    const multiclient::Identity &holder = multiclient::shared_control_token().holder();
    if (result.reason == multiclient::RefusalReason::not_holder) {
        printf("error:Refused -- %.*s has control\r\n", static_cast<int>(holder.name_len), holder.name);
    } else if (holder.identified) {
        printf("error:Refused -- %.*s has control and an interactive move is in progress\r\n",
               static_cast<int>(holder.name_len), holder.name);
    } else {
        printf("error:Refused -- an interactive move is in progress\r\n");
    }
}

// The control-token gate (libs/ControlToken.h): the authoritative call,
// deciding the USB link's own command against the shared control token for
// real. Called from on_main_loop(), at the moment this frame is actually
// about to be dispatched -- the only call of the two (see
// refused_on_arrival() below) that may change holder() and the only one
// after which a control-changed event is published. See
// WifiProvider::gate_dispatch() for the same gate on WiFi, against the same
// multiclient::shared_control_token().
bool SerialConsole::gate_dispatch(const makera::Packet &packet) {
    // Checked before the control gate, so a refused command never moves
    // control either.
    if (packet.type == PTYPE_CTRL_MULTI && multiclient::shared_job_start_wait().pending() &&
        multiclient::refused_while_start_pending(reinterpret_cast<const char*>(packet.data), packet.data_length)) {
        printf("%s", multiclient::job_start_pending_reply);
        return false;
    }

    const GateInputs in = gate_inputs_for(packet);

    const multiclient::GateResult result = multiclient::shared_control_token().gate(
        in.sender, in.traffic, in.motion, multi_client_mode, in.action, multi_client_passive_rights);

    if (result.refused) {
        reply_gate_refusal(result);
        return false;
    }

    if (result.holder_changed) {
        const multiclient::Identity &holder = multiclient::shared_control_token().holder();
        uint8_t payload[1 + 8 + 1 + multiclient::max_name_length];
        const std::size_t length =
            multiclient::build_control_changed_event(holder.id, holder.name, holder.name_len, payload, sizeof(payload));
        if (length != 0) THEKERNEL->streams->publish_multiclient(PTYPE_EVENT, payload, length);
    }

    return true;
}

// The early half of the same gate: ControlToken::peek(), against the
// motion state right now, for a frame that has just arrived -- see
// WifiProvider::refused_on_arrival()'s own comment, which applies here
// unchanged (both links share the same ControlToken and the same race).
bool SerialConsole::refused_on_arrival(const makera::Packet &packet) {
    const GateInputs in = gate_inputs_for(packet);
    const multiclient::GateResult result = multiclient::shared_control_token().peek(
        in.sender, in.traffic, in.motion, multi_client_mode, in.action, multi_client_passive_rights);
    if (!result.refused) return false;
    reply_gate_refusal(result);
    return true;
}

void SerialConsole::dispatch_automatic_command(const makera::Packet &packet) {
    const multiclient::AutomaticCommand command =
        multiclient::classify_automatic_command(packet.data, packet.data_length, THEKERNEL->get_state() == IDLE, true);

    answering_automatic = true;
    if (command == multiclient::AutomaticCommand::refuse) {
        printf("error:Refused -- not allowed as an automatic command\r\n");
    } else {
        struct SerialMessage message;
        message.message.assign(reinterpret_cast<const char *>(packet.data) + 1, packet.data_length - 1);
        message.stream = this;
        message.line = 0;
        THEKERNEL->dispatch_console_line(message);
    }
    answering_automatic = false;
}

// Actual event calling must happen in the main loop because if it happens in the interrupt we will loose data
void SerialConsole::on_main_loop(void * argument){
    if (communication_protocol == PROTOCOL_MAKERA) {
        if (command_waiting && !THEKERNEL->is_dispatching_console_line()) {
            const makera::Packet &packet = makera_frame_decoder.packet();

            // Cleared before the gate, not after: a refused command must
            // not be retried on the next tick, and gate_dispatch() has
            // already sent its own reply by the time it returns.
            command_waiting = false;

            if (packet.type == PTYPE_AUTO_COMMAND) {
                dispatch_automatic_command(packet);
                return;
            }

            // The control-token gate (libs/ControlToken.h) already peeked
            // at this frame, in process_makera_byte(), at the moment it
            // arrived -- see the comment there -- and a frame refused then
            // never reaches here. gate_dispatch() now runs for real,
            // against a fresh motion snapshot: see WifiProvider's matching
            // comment for why the peek is not the last word.
            if (!gate_dispatch(packet)) return;

            struct SerialMessage message;
            message.message.assign(reinterpret_cast<const char *>(packet.data), packet.data_length);
            message.stream = this;
            message.line = 0;

            // Publish the command's own text before dispatching it, tagged
            // with this USB link's identity -- only for an ordinary command
            // (PTYPE_CTRL_MULTI): a file-transfer start (PTYPE_FILE_START)
            // is not text and stays point to point.
            if (packet.type == PTYPE_CTRL_MULTI) {
                publish_console_line(message.message.c_str(), message.message.size());
            }

            THEKERNEL->dispatch_console_line(message);
        }
        return;
    }

    if ( this->has_char('\n') ){
        string received;
        received.reserve(20);
        while(1){
           char c;
           this->buffer.pop_front(c);
           if( c == '\n' ){
                struct SerialMessage message;
                message.message = received;
                message.stream = this;
                message.line = 0;
                THEKERNEL->call_event(ON_CONSOLE_LINE_RECEIVED, &message );
                // this->puts(received.c_str());
                return;
            }else{
                received += c;
            }
        }
    }
}

int SerialConsole::puts(const char* s, int size)
{
    size_t n = size == 0 ? strlen(s) : size;
    // Everything else reaching this link while it must identify first is
    // dropped, including messages sent to every link (libs/Hello.h).
    if (multiclient::must_identify_first(multiclient::shared_client_table(), multiclient::shared_client_table().usb()) &&
        !multiclient::sent_before_identifying(reinterpret_cast<const uint8_t*>(s), n)) return n;
    for (size_t i = 0; i < n; ++i) {
        this->_putc(s[i]);
    }
    return n;
}

int SerialConsole::gets(char** buf, int size)
{
	if (communication_protocol == PROTOCOL_MAKERA) {
        while (makera_rx_bytes.tail != makera_rx_bytes.head || this->ready()) {
            uint8_t received;
            if (makera_rx_bytes.tail != makera_rx_bytes.head) {
                char buffered;
                makera_rx_bytes.pop_front(buffered);
                received = static_cast<uint8_t>(buffered);
            } else {
                const int received_byte = this->read_byte();
                if (received_byte < 0) break;
                received = static_cast<uint8_t>(received_byte);
            }
            uint16_t checksum;

            switch (file_parse_state) {
                case FILE_WAIT_HEADER:
                    file_header[0] = file_header[1];
                    file_header[1] = received;
                    if (((file_header[0] << 8) | file_header[1]) == HEADER) {
                        file_parse_state = FILE_READ_LENGTH;
                        file_bytes_needed = 2;
                        file_frame_index = 0;
                    }
                    break;

                case FILE_READ_LENGTH:
                    xbuff[file_frame_index++] = received;
                    if (--file_bytes_needed == 0) {
                        uint16_t expected_length = (xbuff[0] << 8) | xbuff[1];
                        if (expected_length >= 3 && expected_length <= XBUFF_LENGTH - 2) {
                            file_parse_state = FILE_READ_DATA;
                            file_bytes_needed = expected_length;
                        } else {
                            reset_file_parser();
                        }
                    }
                    break;

                case FILE_READ_DATA:
                    xbuff[file_frame_index++] = received;
                    if (--file_bytes_needed == 0) {
                        file_parse_state = FILE_CHECK_FOOTER;
                        file_bytes_needed = 2;
                    }
                    break;

                case FILE_CHECK_FOOTER:
                    file_footer[0] = file_footer[1];
                    file_footer[1] = received;
                    if (--file_bytes_needed == 0) {
                        checksum = (file_footer[0] << 8) | file_footer[1];
                        int command = checksum == FOOTER ? check_file_packet(buf) : 0;
                        reset_file_parser();
                        if (command != 0) return command;
                    }
                    break;
            }
        }
        return 0;
    }

	getc_result = this->_getc();
	*buf = &getc_result;
	return 1;
}

void SerialConsole::process_makera_byte(uint8_t received)
{
    const makera::DecodeResult result = makera_frame_decoder.decode_byte(received, last_activity_us);
    if (result != makera::DecodeResult::complete) return;

    const makera::Packet &packet = makera_frame_decoder.packet();

    // While another client has identified, this link gets nothing acted on
    // but its hello and its status queries until it identifies too (see
    // libs/Hello.h).
    if (multiclient::must_identify_first(multiclient::shared_client_table(), multiclient::shared_client_table().usb()) &&
        !multiclient::taken_before_identifying(packet.type, packet.data, packet.data_length)) return;

    if (packet.type == PTYPE_CTRL_SINGLE && packet.data_length > 0) {
        switch (makera::handle_control(packet.data[0])) {
            case makera::ControlAction::query: query_flag = true; break;
            case makera::ControlAction::diagnose: diagnose_flag = true; break;
            case makera::ControlAction::halt: halt_flag = true; break;
            default: break;
        }
        return;
    }

    if (packet.type == PTYPE_CTRL_MULTI && makera::is_diagnostic_request(packet.data, packet.data_length)) {
        diagnose_flag = true;
        return;
    }

    if (packet.type == PTYPE_HELLO) {
        handle_hello(packet.data, packet.data_length, last_activity_us);
        return;
    }

    if (packet.type == PTYPE_CLIENT_LIST_REQ) {
        handle_client_list_request();
        return;
    }

    if (packet.type == PTYPE_HEARTBEAT) {
        multiclient::Client *self = multiclient::shared_client_table().usb();
        if (self != nullptr) multiclient::record_heartbeat(*self, last_activity_us);
        return;
    }

    if (packet.type == PTYPE_PRESENCE_REPLY) {
        multiclient::shared_identity_check().note_answer(multiclient::usb_seat(), packet.data, packet.data_length);
        return;
    }

    // Never touches the control token: see WifiProvider's matching case.
    if (packet.type == PTYPE_JOB_START_READY) {
        multiclient::Client *self = multiclient::shared_client_table().usb();
        if (self != nullptr) multiclient::shared_job_start_wait().mark_ready(*self, packet.data, packet.data_length);
        return;
    }

    if (packet.type == PTYPE_RELAY) {
        handle_relay(packet.data, packet.data_length);
        return;
    }

    // Single-user mode never reaches here for this type: unrecognised, and
    // simply ignored, exactly as it is today (0x66 does not exist yet in
    // that mode's behaviour).
    if (packet.type == PTYPE_CONTROL_RELEASE && multi_client_mode == multiclient::Mode::multi_user) {
        handle_control_release();
        return;
    }

    // An automatic command is only taken once this link has identified
    // itself; before that it is ignored, like any other type this firmware
    // does not handle.
    if (packet.type == PTYPE_AUTO_COMMAND &&
        !multiclient::identity_of(multiclient::shared_client_table().usb()).identified) return;

    if (packet.type == PTYPE_CTRL_MULTI || packet.type == PTYPE_FILE_START || packet.type == PTYPE_AUTO_COMMAND) {
        if (packet.data_length == 0) {
            if (packet.type == PTYPE_FILE_START) makera_file_cancel = true;
            return;
        }

        // The control-token gate (libs/ControlToken.h) peeks now, against
        // the motion state right now, for anything but an automatic
        // command -- not only once this frame is finally dispatched.
        // process_makera_byte() is reached from on_idle(), which is
        // itself re-entered while a jog, probe, homing or automatic
        // tool-change move's own dispatch is still running; peeking only
        // once that dispatch ends would check the motion state *after*
        // the move that was supposed to block it. A refused frame gets
        // its reply from refused_on_arrival() and is dropped here, never
        // queued -- but it only ever refuses, never seizes control (see
        // ControlToken::peek()). Whatever it lets through still goes
        // through gate_dispatch(), for real, at its own turn to dispatch
        // below: see WifiProvider's matching comment for why that second,
        // authoritative check is needed and not just belt-and-braces.
        if (packet.type != PTYPE_AUTO_COMMAND && refused_on_arrival(packet)) return;

        command_waiting = true;
#if defined(STREAMED_JOB_PLAYBACK)
    } else if (packet.type >= PTYPE_PLAY_VIEW && packet.type <= PTYPE_GOTO_LINES) {
        player_link_packet link { packet.type, packet.data, packet.data_length };
        PublicData::set_value(player_checksum, link_packet_checksum, &link);
#endif
    }
}

uint8_t SerialConsole::hello_ack_mode() const {
    if (multi_client_mode == multiclient::Mode::multi_user) return multiclient::hello_mode_multi_user;
    return multiclient::hello_mode_single_user;
}

void SerialConsole::send_hello_ack(uint8_t result) {
    uint8_t ack[multiclient::hello_ack_length];
    const std::size_t ack_len = multiclient::build_hello_ack(ack, result, hello_ack_mode(),
                                                             multiclient::shared_job_start_wait().hello_ack_features());
    PacketMessage(PTYPE_HELLO_ACK, reinterpret_cast<const char*>(ack), static_cast<int>(ack_len));
}

// Parses a hello frame and answers it. An already-identified USB link
// re-sending hello is re-acked with no state change. A first-time hello
// while an old (never-identified, window-expired) client is already known
// to be connected -- other than this USB link itself -- is refused. A
// first-time hello whose id an identified WiFi client already has is
// decided by libs/IdentityCheck.h and answered by drive_identity_check():
// from the same launch, WifiProvider removes that client and closes its
// connection, and this link is admitted; from a different launch, that
// client is first asked whether it is still there.
void SerialConsole::handle_hello(const uint8_t* payload, uint16_t payload_length, uint32_t now_us) {
    auto &table = multiclient::shared_client_table();
    multiclient::Client *self = table.usb();
    if (self == nullptr) return;

    multiclient::Hello hello;
    if (!multiclient::parse_hello(payload, payload_length, hello)) return; // malformed, or an unrecognised version: ignored

    if (!self->identified) {
        auto &check = multiclient::shared_identity_check();
        if (check.holds(multiclient::usb_seat())) return; // its first hello is still being decided

        if (table.has_old_client(now_us, -1, /*exclude_usb=*/true)) {
            send_hello_ack(multiclient::hello_result_old_controller_present);
            return;
        }

        const multiclient::HelloDecision decision = multiclient::decide_hello(table, multiclient::usb_seat(), hello, check);
        if (decision.action == multiclient::HelloAction::admit) {
            admit_usb_hello(hello, false);
            return;
        }
        // The id is on WiFi, whose connection only WifiProvider can close,
        // so even a reconnect waits for its idle loop to remove it.
        const bool ask_first = decision.action == multiclient::HelloAction::ask;
        if (decision.action == multiclient::HelloAction::busy ||
            !check.begin(table, multiclient::usb_seat(), decision.other, hello, ask_first, us_ticker_read())) {
            // Another check is running; one runs at a time. The controller
            // sends its hello again shortly.
            multiclient::restart_hello_window(*self, now_us);
            send_hello_ack(multiclient::hello_result_busy);
            return;
        }
        drive_identity_check(us_ticker_read());
        return;
    }
    self->features = hello.features;
    self->has_launch = hello.has_launch;
    self->launch = hello.launch;
    send_hello_ack(multiclient::hello_result_accepted);
}

// Identifies this USB link with `hello` and accepts its hello. A controller
// admitted under the id that holds control never keeps it: control is
// freed and the change published.
void SerialConsole::admit_usb_hello(const multiclient::Hello& hello, bool reconnect) {
    multiclient::Client *self = multiclient::shared_client_table().usb();
    if (self == nullptr) return;

    if (multiclient::admit_hello(*self, hello, multiclient::shared_control_token())) {
        uint8_t payload[1 + 8 + 1];  // holder_id 0 + holder_name_len 0: nobody has control
        const std::size_t length = multiclient::build_control_changed_event(0, nullptr, 0, payload, sizeof(payload));
        if (length != 0) THEKERNEL->streams->publish_multiclient(PTYPE_EVENT, payload, length);
    }

    if (!reconnect) {
        uint8_t joined_payload[1 + 8 + 1 + multiclient::max_name_length];
        const std::size_t joined_length = multiclient::build_client_joined_event(
            self->id, self->name, self->name_len, joined_payload, sizeof(joined_payload));
        if (joined_length != 0) THEKERNEL->streams->publish_multiclient(PTYPE_EVENT, joined_payload, joined_length);
    }
    send_hello_ack(multiclient::hello_result_accepted);
}

// Carries out the USB side of the identity check in progress, if any
// (libs/IdentityCheck.h): asking or clearing this link's identity when it
// is the old entry, and answering this link's held hello. See
// WifiProvider::drive_identity_check() for the WiFi side. Called from
// on_idle() and straight after a check starts.
void SerialConsole::drive_identity_check(uint32_t now_us) {
    auto &check = multiclient::shared_identity_check();
    auto &table = multiclient::shared_client_table();
    for (;;) {
        switch (check.next_step(table, multiclient::Link::usb, now_us)) {
            case multiclient::CheckStep::ask: {
                uint8_t question[multiclient::presence_check_length];
                const std::size_t length = check.build_question(question);
                PacketMessage(PTYPE_PRESENCE_CHECK, reinterpret_cast<const char*>(question), static_cast<int>(length));
                check.asked(now_us);
                break;
            }
            case multiclient::CheckStep::retire:
                table.clear_usb_identity();
                check.retired();
                break;
            case multiclient::CheckStep::refuse:
                send_hello_ack(multiclient::hello_result_identity_connected);
                check.end(table);
                return;
            case multiclient::CheckStep::admit: {
                const multiclient::Hello hello = check.hello();
                check.end(table);
                admit_usb_hello(hello, true);
                return;
            }
            case multiclient::CheckStep::drop:
                check.end(table);
                return;
            default:
                return;
        }
    }
}

// Answers a client-list request with every identified client in the shared
// table. Answered regardless of whether this USB link is itself identified
// -- this is a request-and-reply message, not a publish -- unless it must
// identify first, in which case its request never reaches here (see
// process_makera_byte()).
void SerialConsole::handle_client_list_request() {
    uint8_t payload[multiclient::max_client_list_reply_length];
    const std::size_t length = multiclient::build_client_list_reply(
        multiclient::shared_client_table(), multiclient::shared_control_token(), payload, sizeof(payload));
    PacketMessage(PTYPE_CLIENT_LIST_REPLY, reinterpret_cast<const char*>(payload), static_cast<int>(length));
}

// Hands a relay frame's opaque payload to publish_relay() (via
// THEKERNEL->streams, so it also reaches WiFi), tagged with this USB link's
// own id. Dropped here if this link hasn't identified -- see
// WifiProvider::handle_wifi_relay()'s own comment for why. Never touches
// the control token: handled inline here, never reaching gate_dispatch().
void SerialConsole::handle_relay(const uint8_t* payload, uint16_t payload_length) {
    const multiclient::Client *self = multiclient::shared_client_table().usb();
    if (self == nullptr || !self->identified) return;
    THEKERNEL->streams->publish_relay(self->id, payload, payload_length);
}

// Frees control if this USB link currently holds it, and publishes a
// control-changed event naming nobody if it did -- the same event
// on_second_tick()'s reconcile_holder() call publishes for a disconnect.
// See WifiProvider::handle_wifi_control_release()'s own comment for why an
// unidentified or non-holding sender is simply a no-op here.
void SerialConsole::handle_control_release() {
    const multiclient::Client *self = multiclient::shared_client_table().usb();
    if (self == nullptr || !self->identified) return;
    if (!multiclient::shared_control_token().release_if_holder(self->id)) return;

    uint8_t payload[1 + 8 + 1];  // holder_id 0 + holder_name_len 0: nobody has control
    const std::size_t length = multiclient::build_control_changed_event(0, nullptr, 0, payload, sizeof(payload));
    if (length != 0) THEKERNEL->streams->publish_multiclient(PTYPE_EVENT, payload, length);
}

// Reuses the base StreamOutput::PacketMessage() to build and send the frame
// (this link has only one client, so "targeted" and "broadcast" are the
// same send), then, for an ordinary command reply, also publishes it --
// see WifiProvider::PacketMessage() for the full reasoning behind the
// PTYPE_NORMAL_INFO gate, which is identical here.
void SerialConsole::PacketMessage(char cmd, const char* s, int size) {
    StreamOutput::PacketMessage(cmd, s, size);

    if (cmd == PTYPE_NORMAL_INFO && communication_protocol == PROTOCOL_MAKERA && !answering_automatic) {
        const size_t total_length = size == 0 ? (s == nullptr ? 0 : strlen(s)) : static_cast<size_t>(size);
        publish_console_line(s, total_length);
    }
}

// Publishes `text` as one or more published-console-line fragments, tagged
// with this USB link's own id/name (all-zero/empty if it hasn't
// identified -- see WifiProvider::publish_console_line() for why an
// unidentified sender is still tagged and published, just with nothing
// useful in the tag).
void SerialConsole::publish_console_line(const char* text, size_t length) {
    const multiclient::Client *self = multiclient::shared_client_table().usb();
    if (self == nullptr) return;

    uint8_t frame[8 + 1 + multiclient::max_name_length + 1 + multiclient::max_console_line_text_bytes];
    size_t offset = 0;
    do {
        const size_t chunk_length = multiclient::console_line_chunk_length(length - offset);
        const bool more = multiclient::console_line_has_more(length - offset, chunk_length);
        const size_t frame_length = multiclient::build_console_line_frame(
            self->id, self->name, self->name_len, text + offset, chunk_length, more, frame, sizeof(frame));
        if (frame_length == 0) return; // should not happen: frame is sized for the worst case
        THEKERNEL->streams->publish_multiclient(PTYPE_PUBLISHED_LINE, frame, frame_length);
        offset += chunk_length;
    } while (offset < length);
}

// Reached through THEKERNEL->streams->publish_multiclient() (see
// libs/StreamOutputPool.h). Sends to this USB link's own client, if it is
// identified -- an unidentified one never sees any of this, same as it
// never saw the messages #17 added.
void SerialConsole::publish_multiclient(char cmd, const uint8_t* payload, size_t length) {
    if (communication_protocol != PROTOCOL_MAKERA) return;
    const multiclient::Client *usb = multiclient::shared_client_table().usb();
    if (usb == nullptr || !usb->identified) return;
    StreamOutput::PacketMessage(cmd, reinterpret_cast<const char*>(payload), static_cast<int>(length));
}

// Reached through THEKERNEL->streams->publish_relay() (libs/StreamOutputPool.h).
// Sends to this USB link's own client, if it is identified and is not
// `source_id` itself -- this link has only one client, so "the sender was
// on USB" and "there is nobody left on USB to relay to" are the same case,
// and this returns without sending, same as WifiProvider::publish_relay()
// does for its own sender.
void SerialConsole::publish_relay(uint64_t source_id, const uint8_t* payload, size_t length) {
    if (communication_protocol != PROTOCOL_MAKERA) return;
    const multiclient::Client *usb = multiclient::shared_client_table().usb();
    if (usb == nullptr || !usb->identified || usb->id == source_id) return;
    uint8_t frame[8 + multiclient::max_relay_payload_bytes];
    const size_t frame_length = multiclient::build_relay_frame(source_id, payload, length, frame, sizeof(frame));
    if (frame_length == 0) return;
    StreamOutput::PacketMessage(PTYPE_RELAY, reinterpret_cast<const char*>(frame), static_cast<int>(frame_length));
}

int SerialConsole::receive_packet(makera::Packet& packet, uint32_t timeout_ms)
{
    makera_frame_decoder.reset();
    const uint32_t start_us = us_ticker_read();
    const uint32_t timeout_us = timeout_ms * 1000;
    while (us_ticker_read() - start_us < timeout_us) {
        const int byte = read_byte();
        if (byte == uart_rx_error) {
            makera_frame_decoder.reset();
            return -4;
        }
        if (byte < 0) continue;

        const makera::DecodeResult result = makera_frame_decoder.decode_byte(
            static_cast<uint8_t>(byte), us_ticker_read());
        if (result == makera::DecodeResult::invalid_crc) {
            makera_frame_decoder.reset();
            return -3;
        }
        if (result != makera::DecodeResult::complete) continue;
        packet = makera_frame_decoder.packet();
        makera_frame_decoder.reset();
        return 0;
    }

    makera_frame_decoder.reset();
    return -1;
}

void SerialConsole::reset_file_parser()
{
    file_parse_state = FILE_WAIT_HEADER;
    file_frame_index = 0;
    file_bytes_needed = 2;
    file_header[0] = file_header[1] = 0;
    file_footer[0] = file_footer[1] = 0;
}

int SerialConsole::check_file_packet(char **buf)
{
    if (file_frame_index < 5) return 0;

    uint16_t calculated_crc = crc16::ccitt(xbuff, file_frame_index - 2);
    uint16_t received_crc = (xbuff[file_frame_index - 2] << 8) | xbuff[file_frame_index - 1];
    if (calculated_crc != received_crc) return 0;

    uint8_t command = xbuff[2];
    switch (command) {
        case PTYPE_FILE_MD5:
        case PTYPE_FILE_CAN:
        case PTYPE_FILE_VIEW:
        case PTYPE_FILE_DATA:
        case PTYPE_FILE_END:
        case PTYPE_FILE_RETRY:
        case 0xA0:
        case PTYPE_CTRL_SINGLE:
        case PTYPE_CTRL_MULTI:
            *buf = reinterpret_cast<char *>(xbuff);
            return command;
        default:
            return 0;
    }
}

void SerialConsole::reset()
{
    reset_file_parser();
}

void SerialConsole::on_protocol_changed()
{
    buffer.tail = buffer.head;
    previous_char = 0;
    query_flag = false;
    halt_flag = false;
    diagnose_flag = false;
    makera_file_cancel = false;
    makera_rx_overflow = false;
    command_waiting = false;
    makera_rx_bytes.tail = makera_rx_bytes.head;
    makera_frame_decoder.reset();
    reset_file_parser();
    // A protocol switch invalidates every client's identified state (a
    // fresh hello is required after switching back), and there is no
    // traffic yet under whichever protocol is now in effect, so the hello
    // window has not started either.
    multiclient::shared_client_table().clear_usb_identity();
    // publish_due() is only checked from this file's Makera-mode branch of
    // on_idle(), so last_status_publish_us stops being refreshed for as long
    // as the link stays in Smoothie mode. See WifiProvider::on_protocol_changed()
    // for why that matters and why "now", not 0, is the right value to reset it to.
    last_status_publish_us = us_ticker_read();
}

int SerialConsole::_putc(int c)
{
    return this->serial->putc(c);
}

int SerialConsole::_getc()
{
#if defined(SERIAL_RX_DMA)
    if (rx_lookahead >= 0) {
        const int result = rx_lookahead;
        rx_lookahead = -1;
        return result;
    }
    uint8_t byte = 0;
    return uart2_rx_dma::try_get(byte) ? byte : -1;
#else
    return this->serial->getc();
#endif
}

bool SerialConsole::ready()
{
    if (communication_protocol == PROTOCOL_MAKERA && makera_rx_bytes.tail != makera_rx_bytes.head) return true;
#if defined(SERIAL_RX_DMA)
    if (rx_lookahead >= 0) return true;
    uint8_t byte = 0;
    if (!uart2_rx_dma::try_get(byte)) return false;
    rx_lookahead = byte;
    return true;
#else
    return this->serial->readable();
#endif
}

#if defined(SERIAL_RX_DMA)
bool SerialConsole::handle_rx_error()
{
    if (!uart2_rx_dma::take_error()) return false;

    rx_lookahead = -1;
    makera_rx_bytes.tail = makera_rx_bytes.head;
    makera_rx_overflow = false;
    buffer.tail = buffer.head;
    previous_char = 0;
    makera_frame_decoder.reset();
    reset_file_parser();
    THEKERNEL->streams->printf("ERROR: UART receive error or overflow; buffered input discarded\n");
    return true;
}
#endif

int SerialConsole::read_byte()
{
#if defined(SERIAL_RX_DMA)
    if (handle_rx_error()) return uart_rx_error;
    return _getc();
#else
    return this->serial->readable() ? this->serial->getc() : -1;
#endif
}

// Does the queue have a given char ?
bool SerialConsole::has_char(char letter){
    int index = this->buffer.tail;
    while( index != this->buffer.head ){
        if( this->buffer.buffer[index] == letter ){
            return true;
        }
        index = this->buffer.next_block_index(index);
    }
    return false;
}
