// The custom composite HID device and the USB link state. ESP32 only.
#pragma once

#include "hid.h"

namespace mp {

const char* usb_serial();  // the USB serial number the host sees

void usb_begin();
bool usb_up();               // mounted and not suspended
bool usb_take_disturbed();   // a loss or re-enumeration since the last call
UsbPort& usb_port();
void usb_detach(bool detach);  // self-test: soft disconnect and reconnect

// True when no IN endpoint has a transfer waiting for its FIFO-empty
// interrupt. The core's USB driver (dcd_esp32sx) sets bits in that shared mask
// register with an unguarded read-modify-write, and its interrupt handler on
// the other CPU core clears them the same way, so two IN transfers started
// close together can lose a bit and leave an endpoint stuck. Every IN transfer
// we start (HID reports, CDC packets) therefore waits until the mask is clear.
bool usb_in_idle();

// When the last HID report was accepted (micros). Serial output waits for HID
// to be quiet: the core's USB driver can't safely interleave HID and CDC IN
// transfers (arduino-esp32 issue #10307, TinyUSB #4159).
uint32_t usb_last_hid_us();

}  // namespace mp
