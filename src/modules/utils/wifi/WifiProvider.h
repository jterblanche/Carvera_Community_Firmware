/*
 * WifiProvider.h
 *
 *  Created on: 2020年6月10日
 *      Author: josh
 */

#ifndef WIFIPROVIDER_H_
#define WIFIPROVIDER_H_

using namespace std;
#include <string>
#include <vector>

#include "Pin.h"
#include "Module.h"
#include "StreamOutput.h"

#include "M8266WIFIDrv.h"
#include "libs/RingBuffer.h"
#include "libs/MakeraFrame.h"
#include "libs/ClientTable.h"
#include "libs/Hello.h"
#include "libs/Publish.h"

#define WIFI_DATA_MAX_SIZE 1460
#define WIFI_DATA_TIMEOUT_MS 10
#define MAX_WLAN_SIGNALS 8

enum ParseState { WAIT_HEADER, READ_LENGTH, READ_DATA, CHECK_FOOTER };

class WifiProvider : public Module, public StreamOutput
{
public:
	WifiProvider();

    void on_module_loaded();
    void on_gcode_received(void *argument);
    void on_main_loop( void* argument );
    void on_second_tick(void* argument);
    void on_idle(void* argument);
    void on_get_public_data(void* argument);
    void on_set_public_data(void* argument);
    void on_protocol_changed();
    void publish_multiclient(char cmd, const uint8_t* payload, size_t length);

    int gets(char** buf, int size = 0);
    int puts(const char*, int size = 0);
    int _putc(int c);
    int _getc(void);
    bool ready();
    bool has_char(char letter);
    int type(); // 0: serial, 1: wifi
    ProtocolMode protocol();
    void reset(void){ptrData=0;ptr_xbuff=0;currentState = WAIT_HEADER;};
    int printfcmd(const char cmd, const char *format, ...);
    int printf(const char *format, ...) __attribute__ ((format(printf, 2, 3)));


private:
    void M8266WIFI_Module_delay_ms(u16 nms);
    void set_wifi_op_mode(u8 op_mode);

    void M8266WIFI_Module_Hardware_Reset(void);
    u8 M8266WIFI_Module_Init_Via_SPI();

    void init_wifi_module(bool reset);
    void query_wifi_status();
    void update_ap_auto_disable(u8 connection_status);

    uint32_t ip_to_int(const char* ip_addr);
    void int_to_ip(uint32_t i_ip, char *ip_addr);
    void get_broadcast_from_ip_and_netmask(char *broadcast_addr, char *ip_addr, char *netmask);

    void on_pin_rise();
    void receive_wifi_data();
    int CheckFilePacket(char** buf);

    void PacketMessage(char cmd, const char* s, int size);

    // Makera-mode per-client routing. `client_index` is a slot in
    // `wifi_streams`/the shared ClientTable's WiFi entries, in [0, max_wifi_clients).
    int route_makera_client(const u8 remote_ip[4], u16 remote_port, uint32_t now_ms);
    void disconnect_wifi_client(const multiclient::Address& address, const char* reason, bool log = true);
    void send_to_wifi_client(int client_index, const u8* data, size_t length);
    void broadcast_to_wifi_clients(const u8* data, size_t length);
    void broadcast_to_identified_wifi_clients(const u8* data, size_t length);
    void reconcile_wifi_clients(uint8_t client_num, ClientInfo remote_clients[]);
    void forget_wifi_client(int client_index);
    void enforce_old_client_rule(uint32_t now_ms);

    // Handles a decoded hello/client-list-request frame. Both reply inline,
    // addressed to that one client (see receive_wifi_data()).
    void handle_wifi_hello(int client_index, const uint8_t* payload, uint16_t payload_length, uint32_t now_ms);
    void handle_wifi_client_list_request(int client_index);
    // Sends a framed reply addressed to one specific client, regardless of
    // whatever active_reply_client currently holds (saves and restores it).
    void send_wifi_packet(int client_index, char cmd, const uint8_t* payload, size_t length);

    // Proactive status publish (contract section 6.9, "reuses 0x81"), at
    // the configured rate, to every identified WiFi client -- independent
    // of any client's own query_flag poll, which is still answered
    // separately (on_idle). Already inherits the existing "no status while
    // uploading" pause, since this runs from on_idle which bails out on
    // THEKERNEL->is_uploading() before either mechanism runs.
    void publish_status_if_due(uint32_t now_ms);

    // Publishes `text` (a command's own text, or its reply) as one or more
    // published-console-line (0x69) fragments, tagged with client_index's
    // id/name, to every identified client across every transport (WiFi and
    // USB) -- see StreamOutput::publish_multiclient().
    void publish_console_line(int client_index, const char* text, size_t length);

    // Sends the wire-framed bytes built from `cmd`+`payload` to every
    // identified WiFi client. The framing helper both publish_console_line
    // (via THEKERNEL->streams->publish_multiclient(), which calls back into
    // this override) and publish_status_if_due() ultimately go through.
    void send_framed_to_identified_wifi_clients(char cmd, const uint8_t* payload, size_t length);

    // multi_client.status_publish_hz, converted once at load time
    // (on_module_loaded) from the configured rate.
    uint32_t status_publish_interval_ms;
    uint32_t last_status_publish_ms = 0;

    mbed::InterruptIn *wifi_interrupt_pin; // Interrupt pin for measuring speed

    RingBuffer<char, 256> buffer; // Receive buffer

	u8 WifiData[WIFI_DATA_MAX_SIZE];

	int tcp_port;
	int udp_send_port;
	int udp_recv_port;
	int tcp_timeout_s;
	int max_clients;
	int connection_fail_count;
	int sta_down_seconds;
	uint32_t wifi_seconds;
	uint32_t sta_flap_times[3]; // timestamps of recent STA reconnect cycles (WIFI_STA_FLAP_LIMIT)
	uint32_t ap_hold_remaining_s; // keep AP up while > 0 after STA flapping
	u8 last_sta_connection_status;
	uint8_t sta_flap_count;
	char machine_name[64]; // Fixed-size buffer to avoid std::string heap allocation
	char ap_address[16];
	char ap_netmask[16];
	char sta_address[16];
	char sta_netmask[16];

    struct {
    	u8  tcp_link_no;
    	u8  udp_link_no;
    	bool wifi_init_ok:1;
    	bool ap_auto_disable:1;
    	bool ap_currently_on:1;
    	bool ap_manually_disabled:1; // sticky from `ap disable` until `ap enable`
    	bool sta_was_connected:1;
    	bool sta_down_since_connected:1;
    	volatile bool halt_flag:1;      // Smoothie mode only; Makera mode broadcasts on halt instead (see puts())
    	volatile bool query_flag:1;     // Smoothie mode only; Makera mode uses wifi_streams[i].query_flag
    	volatile bool diagnose_flag:1;  // Smoothie mode only; Makera mode uses wifi_streams[i].diagnose_flag
    	volatile bool has_data_flag:1;
		bool command_waiting:1;         // Makera mode: some client's command is decoded and awaiting dispatch
    };
    bool makera_file_cancel;
    ParseState currentState = WAIT_HEADER;
    int ptrData;
    int ptr_xbuff;

    // Makera mode: one frame decoder per WiFi client, so an interleaved
    // second client can never corrupt another client's in-progress frame.
    // Only one command is dispatched at a time (matching the kernel's own
    // sequential dispatch), so `command_waiting`/`command_waiting_client`
    // stay single-valued rather than one per client.
    struct WifiClientStream {
    	makera::Packet packet{};
    	makera::FrameDecoder decoder{packet};
    	bool query_flag = false;
    	bool diagnose_flag = false;
    	uint16_t header_errors = 0;

    	// FrameDecoder holds a reference to `packet`, so this struct has no
    	// copy/move assignment; reset it in place instead.
    	void clear() {
    		decoder.reset();
    		query_flag = false;
    		diagnose_flag = false;
    		header_errors = 0;
    	}
    };
    WifiClientStream wifi_streams[multiclient::max_wifi_clients];

    // Index into wifi_streams for the client a reply is being sent to right
    // now (set for the duration of a dispatch or a query/diagnose reply),
    // or -1 when nothing specific is being answered -- puts() then
    // broadcasts, which is what a halt notice or an unprompted kernel
    // message wants.
    int active_reply_client = -1;
    int command_waiting_client = -1;
    int makera_file_cancel_client = -1;

    // A read in receive_wifi_data() can hold more than one frame from the
    // same client (the controller doesn't wait for "ok" before sending its
    // next command). When a completed command breaks out of the byte loop,
    // any bytes already read past it -- still sitting in WifiData -- are
    // saved here and replayed before the next RecvData_ex call, instead of
    // being dropped. pending_wifi_client is -1 when there is nothing
    // pending.
    int pending_wifi_client = -1;
    uint16_t pending_wifi_offset = 0;
    uint16_t pending_wifi_count = 0;

    // Addresses already logged as refused-but-still-present by
    // reconcile_wifi_clients(), so a disconnect call that the WiFi module
    // doesn't honour doesn't repeat the same log line every second. See
    // reconcile_wifi_clients() in WifiProvider.cpp.
    static constexpr size_t max_logged_refusals = 4;
    multiclient::Address logged_refusals[max_logged_refusals];
    uint8_t logged_refusal_count = 0;

    // Same idea as logged_refusals above, but for enforce_old_client_rule():
    // an old client whose driver-level disconnect doesn't immediately take
    // is logged only once, not every second it lingers.
    static constexpr size_t max_logged_old_client_disconnects = multiclient::max_wifi_clients;
    multiclient::Address logged_old_client_disconnects[max_logged_old_client_disconnects];
    uint8_t logged_old_client_disconnect_count = 0;
};

#endif /* WIFIPROVIDER_H_ */
