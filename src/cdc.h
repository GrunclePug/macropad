// The USB CDC serial port, driven directly through TinyUSB. ESP32 only.
//
// The core starts its own Serial (USBCDC) before setup(); cdc_begin() ends it,
// which turns the core's CDC callbacks into no-ops (no per-byte RX queue that
// can block the USB task, no DTR/RTS reboot sequences). From then on all
// serial I/O goes through the Link here, and nothing may call Serial.
#pragma once

#include "link.h"

namespace mp {

void cdc_begin();
Link& cdc_link();

// HID and CDC IN traffic never overlap (see usb.h):
// - serial packets start only when HID has been quiet for 2 ms and the report
//   arbiter has nothing queued (the app reports that each loop)
// - a HID report waits while a serial packet is in flight, for at most 1.5 ms,
//   so a host that never reads can't hold keys back
void cdc_set_hid_idle(bool idle);
// The raw control lines, for the knock (a variable read, no endpoint traffic).
void cdc_line_state(bool& dtr, bool& rts);
// True while a CDC IN transfer is still in flight.
bool cdc_tx_busy();
uint32_t cdc_call_count();  // CDC endpoint calls so far (self-test)
bool cdc_tx_holds_hid(uint32_t now_us);

// Paced text: buffered (16 KB) and drained into the link's log as room allows,
// so long reports and frame dumps go out whole without blocking. Dropped only
// if the buffer is full.
void text_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void text_service();
bool text_idle();
void text_clear();  // drop buffered text (self-test report re-send)

}  // namespace mp
