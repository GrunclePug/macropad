#include "display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Wire.h>

#include "cdc.h"

namespace mp {
namespace {

constexpr uint8_t ADDR = 0x3C;
constexpr int W = 128, H = 64;
constexpr uint32_t FRAME_MS = 33;
constexpr uint32_t PROBE_MS = 2000;

struct Frame {
  uint8_t buf[W * H / 8];  // GFXcanvas1 layout: rows, MSB = leftmost pixel
};

GFXcanvas1* canvas = nullptr;
QueueHandle_t queue = nullptr;
volatile bool online = false;
uint32_t drawn_version = 0;
uint8_t drawn_held = 0;
uint32_t drawn_at = 0;
bool drawn_banner = false;

// The driver's boot configuration minus "display off", for re-sending after a
// silent panel reset (it comes back with its defaults: display off and the
// image mirrored). Every command here is safe to repeat.
const uint8_t RECONFIG[] = {
    0xD5, 0x80,  // clock
    0xA8, 0x3F,  // multiplex 64
    0xD3, 0x00,  // display offset
    0x40,        // start line
    0x8D, 0x14,  // charge pump on
    0x20, 0x00,  // horizontal addressing
    0xA1,        // segment remap
    0xC8,        // COM scan descending
    0xDA, 0x12,  // COM pins
    0x81, 0xCF,  // contrast
    0xD9, 0xF1,  // precharge
    0xDB, 0x40,  // VCOMH
    0xA4,        // resume from RAM
    0xA6,        // normal display
    0x2E,        // scrolling off
    0xAF,        // display on
};

const char* text(const char* s) { return s ? s : ""; }

bool probe() {
  Wire.beginTransmission(ADDR);
  return Wire.endTransmission() == 0;
}

// GFXcanvas1 rows (MSB-first) to SSD1306 pages (8 rows per byte, LSB on top).
void convert(const uint8_t* src, uint8_t* dst) {
  memset(dst, 0, W * H / 8);
  for (int y = 0; y < H; y++)
    for (int xb = 0; xb < W / 8; xb++) {
      uint8_t b = src[y * (W / 8) + xb];
      if (!b) continue;
      for (int bit = 0; bit < 8; bit++)
        if (b & (0x80 >> bit)) dst[(y / 8) * W + xb * 8 + bit] |= 1u << (y & 7);
    }
}

void task(void*) {
  static Frame latest;
  static Frame in;
  bool have = false, dirty = false;
  uint32_t next_probe = 0;
  Adafruit_SSD1306 oled(W, H, &Wire, -1);
  Wire.begin();
  Wire.setTimeOut(20);

  for (;;) {
    if (xQueueReceive(queue, &in, pdMS_TO_TICKS(250)) == pdTRUE) {
      memcpy(&latest, &in, sizeof latest);
      have = dirty = true;
    }
    uint32_t now = millis();
    if (due(now, next_probe)) {
      next_probe = now + PROBE_MS;
      bool ack = probe();
      if (ack && !online) {
        // Found (at boot or after coming back): full init.
        online = oled.begin(SSD1306_SWITCHCAPVCC, ADDR, false, false);
        dirty = have;
      } else if (ack) {
        for (uint8_t c : RECONFIG) oled.ssd1306_command(c);
        dirty = have;
      } else {
        online = false;
      }
    }
    if (online && dirty) {
      convert(latest.buf, oled.getBuffer());
      oled.display();
      dirty = false;
    }
  }
}

void print_right(const char* s, int y) {
  canvas->setCursor(W - 6 * (int)strlen(s), y);
  canvas->print(s);
}

const char* title(const Core& c) {
  if (c.in_menu()) return "Menu";
  switch (c.screen()) {
    case Core::Screen::Keys:
      return c.variant().keymaps[c.set()].name;
    case Core::Screen::SetList:
      return "Key sets";
    case Core::Screen::Tool:
      return c.variant().tools[c.tool()]->schema().name;
    case Core::Screen::Configure:
      return c.recovery() ? "Recovery" : "Configure";
  }
  return "";
}

void list(const char* const* names, uint8_t n, uint8_t cur, int y) {
  GFXcanvas1& g = *canvas;
  constexpr uint8_t ROWS = 6;
  uint8_t first = cur >= ROWS ? cur - ROWS + 1 : 0;
  for (uint8_t i = first; i < n && i < first + ROWS; i++, y += 8) {
    if (i == cur) {
      g.fillRect(0, y - 1, W, 9, 1);
      g.setTextColor(0);
    }
    g.setCursor(2, y);
    g.print(names[i]);
    g.setTextColor(1);
  }
}

void render(const Core& c, uint32_t now) {
  const Variant& v = c.variant();
  GFXcanvas1& g = *canvas;
  g.fillScreen(0);
  g.setTextSize(1);
  g.setTextColor(1);
  g.setTextWrap(false);

  // Top bar: screen name, then status on the right.
  g.setCursor(0, 0);
  g.print(title(c));
  char status[16];
  uint8_t n = 0;
  if (!c.usb_up()) status[n++] = '?';
  if (c.unsaved()) status[n++] = '!';  // "Default (not saved)"
  for (uint8_t t = 0; t < v.ntools && n < 10; t++) {
    if (!c.enabled(t)) continue;
    char ch = v.tools[t]->schema().name[0];
    status[n++] = c.active(t) ? ch : (char)(ch | 0x20);  // armed: lower case
  }
  status[n] = 0;
  if (c.killed()) {
    print_right("KILL", 0);
    g.setCursor(W - 6 * (4 + n + 1), 0);
    g.print(status);
  } else {
    print_right(status, 0);
  }
  g.drawFastHLine(0, 10, W, 1);

  int y = 14;
  if (const char* b = c.banner(now)) {
    g.setCursor((W - 6 * (int)strlen(b)) / 2, 32);
    g.print(b);
    return;
  }

  if (c.in_menu()) {
    const char* names[MAX_TOOLS + 2];
    for (uint8_t i = 0; i < c.menu_items(); i++) names[i] = c.menu_name(i);
    list(names, c.menu_items(), c.cursor(), y);
    return;
  }

  switch (c.screen()) {
    case Core::Screen::SetList: {
      const char* names[MAX_KEYMAPS];
      for (uint8_t i = 0; i < v.nkeymaps; i++) names[i] = v.keymaps[i].name;
      list(names, v.nkeymaps, c.list_cursor(), y);
      return;
    }
    case Core::Screen::Configure:
      g.setCursor(0, 24);
      g.print(c.status());
      if (c.unsaved()) {
        g.setCursor(0, H - 8);
        g.print("Default (not saved)");
      }
      return;
    case Core::Screen::Tool: {
      uint8_t ti = c.tool();
      Tool* t = v.tools[ti];
      const ToolSchema& s = t->schema();
      g.setCursor(0, y);
      g.print(c.active(ti) ? "Running" : c.enabled(ti) ? "Armed" : "Off");
      y += 10;
      for (uint8_t i = 0; i < s.nparams && y < H; i++, y += 9) {
        const ParamDef& d = s.params[i];
        int32_t x = t->slot() ? t->slot()[i] : 0;
        g.setCursor(0, y);
        g.print(i == c.focus() ? ">" : " ");
        g.print(d.label);
        char val[16];
        if (d.kind == P_CHOICE)
          snprintf(val, sizeof val, "%s", d.names[x]);
        else if (d.kind == P_KEY && key_name((uint8_t)x))
          snprintf(val, sizeof val, "%s", key_name((uint8_t)x));
        else if (d.kind == P_KEY)
          snprintf(val, sizeof val, "0x%02lX", (long)x);
        else
          snprintf(val, sizeof val, "%ld %s", (long)x, d.unit);
        print_right(val, y);
      }
      return;
    }
    case Core::Screen::Keys:
      break;
  }

  const Keymap& km = v.keymaps[c.set()];
  if (!km.legend && c.live(c.set())) {
    // Live keys: [ ] or [#] per key being sent, at the size of the
    // low-latency letters, rows of four. At text size 2 "[#]" would be 36 px
    // wide, so the glyphs are placed by hand: '[' and ']' are narrow (ink in
    // columns 1-3 of 5), which packs a box into about 22 px.
    uint8_t held = c.held_keys();
    g.setTextSize(2);
    for (uint8_t k = 0; k < v.keys; k++) {
      int x = 6 + (k % 4) * 30;
      int yy = v.keys <= 4 ? 28 : 18 + (k / 4) * 20;
      g.setCursor(x, yy);
      g.print('[');
      if (held & (1u << k)) {
        g.setCursor(x + 10, yy);
        g.print('#');
      }
      g.setCursor(x + 16, yy);
      g.print(']');
    }
    g.setTextSize(1);
    if (v.keys <= 4) {
      g.setCursor(0, H - 8);
      g.print("Live keys");
    }
    return;
  }
  if (!km.legend) {
    // Low latency: a static screen, nothing redraws while keys are played.
    g.setTextSize(2);
    y = 28;
    int x = (W - 4 * 24) / 2;
    for (uint8_t k = 0; k < v.keys && k < 4; k++, x += 24) {
      g.setCursor(x, y);
      g.print(text(km.keys[k].label));
    }
    g.setTextSize(1);
    g.setCursor(0, H - 8);
    g.print("Low latency");
    return;
  }
  for (uint8_t k = 0; k < v.keys; k++) {
    g.setCursor((k % 2) * 64, y + (k / 2) * 9);
    g.print(k + 1);
    g.print(" ");
    g.print(text(km.keys[k].label));
  }
  if (v.encoder) {
    g.setCursor(0, H - 8);
    g.print(text(km.enc_ccw.label));
    g.print(" / ");
    g.print(text(km.enc_cw.label));
    g.print(" / ");
    g.print(text(km.enc_push.label));
  }
}

}  // namespace

void display_begin() {
  canvas = new GFXcanvas1(W, H);
  queue = xQueueCreate(1, sizeof(Frame));
  xTaskCreatePinnedToCore(task, "oled", 4096, nullptr, 1, nullptr, 0);
}

void display_service(const Core& c, uint32_t now, bool force) {
  bool banner = c.banner(now) != nullptr;
  // Key presses only redraw on a keymap with live keys shown.
  const Variant& v = c.variant();
  bool live = c.keymap_live() && !v.keymaps[c.set()].legend && c.live(c.set());
  uint8_t held = live ? c.held_keys() : 0;
  if (!force && c.version() == drawn_version && banner == drawn_banner && held == drawn_held) return;
  if (!force && now - drawn_at < FRAME_MS) return;
  render(c, now);
  drawn_version = c.version();
  drawn_banner = banner;
  drawn_held = held;
  drawn_at = now;
  static Frame f;
  memcpy(f.buf, canvas->getBuffer(), sizeof f.buf);
  xQueueOverwrite(queue, &f);
}

bool display_online() { return online; }

void display_dump() {
  const uint8_t* b = canvas->getBuffer();
  for (int y = 0; y < H; y++) {
    char line[W + 1];
    for (int x = 0; x < W; x++) line[x] = (b[y * (W / 8) + x / 8] & (0x80 >> (x & 7))) ? '#' : '.';
    line[W] = 0;
    text_printf("%s", line);
  }
}

}  // namespace mp
