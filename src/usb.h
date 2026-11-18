// The custom composite HID device and the USB link state. ESP32 only.
#pragma once

#include "hid.h"

namespace mp {

void usb_begin();
bool usb_up();               // mounted and not suspended
bool usb_take_disturbed();   // a loss or re-enumeration since the last call
UsbPort& usb_port();
void usb_detach(bool detach);  // self-test: soft disconnect and reconnect

}  // namespace mp
