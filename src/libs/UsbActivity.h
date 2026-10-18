#pragma once

#include <cstdint>

#include "ClientTable.h"

namespace multiclient {

// How long the USB link can go with nothing received before a temporary baud
// rate set with the baud command reverts to the configured one.
constexpr uint32_t temporary_baud_timeout_us = 15000000;

// When the USB link last received anything, for the two checks that act on a
// quiet link: ending the USB session (usb_session_expired()) and reverting a
// temporary baud rate.
//
// A file transfer over USB switches the link's receive handling off and reads
// the bytes itself, so nothing updates last_activity_us while it runs, although
// the link is busy. Both checks are skipped while receive handling is off, and
// switching it back on counts as activity, so the checks count from the end of
// the transfer. This matters because the machine's idle work runs again while
// an uploaded .lz file is decompressed, before receive handling is back on.
struct UsbActivity {
  // A raw us_ticker_read() reading. On the Carvera it is written from the
  // receive interrupt.
  uint32_t last_activity_us = 0;
  bool rx_detached = false;

  void set_rx_enabled(bool enabled, uint32_t now_us) {
    if (enabled) last_activity_us = now_us;
    rx_detached = !enabled;
  }

  bool session_expired(bool hello_window_started, uint32_t now_us) const {
    return !rx_detached && usb_session_expired(hello_window_started, now_us, last_activity_us);
  }

  // Signed, as in usb_session_expired(): the receive interrupt can stamp the
  // link after now_us was read, and that must count as no quiet time.
  bool temporary_baud_expired(uint32_t now_us) const {
    return !rx_detached &&
           static_cast<int32_t>(now_us - last_activity_us) >= static_cast<int32_t>(temporary_baud_timeout_us);
  }
};

}  // namespace multiclient
