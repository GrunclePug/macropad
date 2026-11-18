#include "cdc.h"

#include <Arduino.h>
#include <stdarg.h>

#include "device/usbd_pvt.h"
#include "tusb.h"
#include "usb.h"

namespace mp {
namespace {

// The CDC bulk IN endpoint, found once in the core's configuration descriptor.
uint8_t cdc_in_ep() {
  static uint8_t ep = 0;
  if (ep) return ep;
  const uint8_t* d = tud_descriptor_configuration_cb(0);
  if (!d) return 0;
  uint16_t total = d[2] | (d[3] << 8);
  bool in_data_itf = false;
  for (uint16_t i = 0; i + 1 < total && d[i]; i += d[i]) {
    if (d[i + 1] == TUSB_DESC_INTERFACE) in_data_itf = d[i + 5] == TUSB_CLASS_CDC_DATA;
    if (d[i + 1] == TUSB_DESC_ENDPOINT && in_data_itf && (d[i + 2] & 0x80)) {
      ep = d[i + 2];
      break;
    }
  }
  return ep;
}

// Sends at most one packet at a time, and only when nothing else is
// transmitting (see usb_in_idle). A packet is under 64 bytes and the FIFO is
// empty when it starts, so TinyUSB's own task never starts a follow-up or
// zero-length transfer on this endpoint: every CDC IN transfer starts here.
constexpr uint32_t PACKET = 63;
constexpr uint32_t HID_QUIET_US = 2000;
constexpr uint32_t HOLD_HID_MAX_US = 1500;

bool hid_idle = true;
uint32_t tx_started_us = 0;

uint32_t calls = 0;  // CDC endpoint calls, for the self-test's normal-mode check

struct TinyCdc : CdcPort {
  // Raw DTR from the line state: tud_cdc_n_connected() also goes false on
  // suspend, which is a USB event, not a session break.
  bool connected() override { return tud_cdc_n_get_line_state(0) & 1; }
  uint32_t available() override {
    calls++;
    return tud_cdc_n_available(0);
  }
  uint32_t read(uint8_t* b, uint32_t n) override {
    calls++;
    return tud_cdc_n_read(0, b, n);
  }
  uint32_t write_available() override {
    calls++;
    uint8_t ep = cdc_in_ep();
    if (!ep || !tud_ready() || usbd_edpt_busy(0, ep) || !usb_in_idle()) return 0;
    if (!hid_idle || micros() - usb_last_hid_us() < HID_QUIET_US) return 0;
    if (tud_cdc_n_write_available(0) < CFG_TUD_CDC_TX_BUFSIZE) return 0;  // FIFO not empty
    return PACKET;
  }
  uint32_t write(const uint8_t* b, uint32_t n) override {
    return tud_cdc_n_write(0, b, n < PACKET ? n : PACKET);
  }
  void flush() override {
    if (tud_cdc_n_write_available(0) < CFG_TUD_CDC_TX_BUFSIZE && tud_cdc_n_write_flush(0))
      tx_started_us = micros();
  }
  void purge() override {
    calls++;
    tud_cdc_n_read_flush(0);
    tud_cdc_n_write_clear(0);
  }
};

TinyCdc port;
Link link(port);

constexpr uint32_t TEXT_CAP = 16384;
char text[TEXT_CAP];
uint32_t text_len = 0;

}  // namespace

void cdc_begin() { Serial.end(); }

void cdc_set_hid_idle(bool idle) { hid_idle = idle; }

void cdc_line_state(bool& dtr, bool& rts) {
  uint8_t s = tud_cdc_n_get_line_state(0);
  dtr = s & 1;
  rts = s & 2;
}

uint32_t cdc_call_count() { return calls; }

bool cdc_tx_busy() {
  uint8_t ep = cdc_in_ep();
  return ep && usbd_edpt_busy(0, ep);
}

bool cdc_tx_holds_hid(uint32_t now_us) {
  uint8_t ep = cdc_in_ep();
  return ep && usbd_edpt_busy(0, ep) && now_us - tx_started_us < HOLD_HID_MAX_US;
}

Link& cdc_link() { return link; }

void text_printf(const char* fmt, ...) {
  char line[Link::MAX_LINE];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  // One buffered line per newline-free chunk; split on embedded newlines.
  const char* p = line;
  while (*p) {
    const char* nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (text_len + len + 1 > TEXT_CAP) return;  // full: drop
    memcpy(text + text_len, p, len);
    text[text_len + len] = 0;
    text_len += len + 1;
    p += len + (nl ? 1 : 0);
  }
}

void text_service() {
  while (text_len) {
    uint16_t len = (uint16_t)strlen(text);
    if (!link.log_room(len) || !link.log(text)) return;
    memmove(text, text + len + 1, text_len - len - 1);
    text_len -= len + 1;
  }
}

bool text_idle() { return text_len == 0; }

}  // namespace mp
