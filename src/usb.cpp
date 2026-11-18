#include "usb.h"

#include <Arduino.h>

#include "USB.h"
#include "USBHID.h"
#include "tusb.h"

namespace mp {
namespace {

// One HID interface, three reports. Each top-level collection opens with Usage
// Page, Usage, Collection (Application), so the core's descriptor parser
// accepts it.
const uint8_t REPORT_DESC[] = {
    // 1: NKRO keyboard: modifier byte plus a bitmap of usages 0x00-0x7F.
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, REPORT_KEYBOARD,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x7F, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x80, 0x81, 0x02,
    0xC0,
    // 2: mouse: 8 buttons, then x, y and wheel as signed bytes.
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, REPORT_MOUSE, 0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06,
    0xC0, 0xC0,
    // 3: consumer control: one bit per usage, in CC_* order, plus padding.
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, REPORT_CONSUMER,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x07,
    0x09, 0xE9, 0x09, 0xEA, 0x09, 0xE2, 0x09, 0xCD, 0x09, 0xB5, 0x09, 0xB6, 0x09, 0xB7,
    0x81, 0x02,
    0x95, 0x01, 0x81, 0x03,
    0xC0,
};

class Device : public USBHIDDevice {
 public:
  Device() {
    // USB.begin() runs before setup() on this board, so the device has to be
    // registered here, during static initialization, as the core's own HID
    // classes do.
    static bool done = false;
    if (!done) {
      done = true;
      hid.addDevice(this, sizeof REPORT_DESC);
    }
  }
  uint16_t _onGetDescriptor(uint8_t* dst) override {
    memcpy(dst, REPORT_DESC, sizeof REPORT_DESC);
    return sizeof REPORT_DESC;
  }
  USBHID hid;
};

Device device;

bool disturbed = false;  // set from the USB event task, taken atomically
volatile uint32_t mounts = 0;

void on_usb(void*, esp_event_base_t base, int32_t id, void*) {
  if (base != ARDUINO_USB_EVENTS) return;
  switch (id) {
    case ARDUINO_USB_STARTED_EVENT:
      if (mounts++ > 0) __atomic_store_n(&disturbed, true, __ATOMIC_SEQ_CST);  // re-enumerated
      break;
    case ARDUINO_USB_STOPPED_EVENT:
    case ARDUINO_USB_SUSPEND_EVENT:
      __atomic_store_n(&disturbed, true, __ATOMIC_SEQ_CST);
      break;
    default:
      break;
  }
}

struct Port : UsbPort {
  bool ready() override { return tud_hid_n_ready(0); }
  bool submit(uint8_t id, const uint8_t* data, uint8_t len) override {
    return tud_hid_n_report(0, id, data, len);
  }
};
Port port;

}  // namespace

void usb_begin() {
  if (tud_mounted()) mounts = 1;
  USB.onEvent(on_usb);
}

bool usb_up() { return tud_mounted() && !tud_suspended(); }

bool usb_take_disturbed() { return __atomic_exchange_n(&disturbed, false, __ATOMIC_SEQ_CST); }

UsbPort& usb_port() { return port; }

void usb_detach(bool detach) {
  if (detach)
    tud_disconnect();
  else
    tud_connect();
}

}  // namespace mp
