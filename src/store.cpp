#include "store.h"

#include <Arduino.h>
#include <Preferences.h>
#include <nvs.h>

namespace mp {
namespace {

constexpr uint32_t IDLE_MS = 1000;
constexpr uint32_t RETRY_MS = 5000;

#ifdef MACROPAD_SELFTEST
const char* const NS = "macropad-test";
#else
const char* const NS = "macropad";
#endif

Preferences prefs;
bool open_ = false;
bool retrying = false;
uint32_t retry_at = 0;
bool old_keys_cleared = false;
uint32_t saved_crc = 0;

// Per-parameter keys written by firmware 0.2.0, erased after the first save.
const char* const OLD_KEYS[] = {"click.min", "click.max", "click.btn", "jitter.dist", "jitter.min",
                                "jitter.max", "spam.set",  "spam.min",  "spam.max",    "spam.hold",
                                "hold.btn",   "osu.live",  "deck.live"};

}  // namespace

void store_begin() { open_ = prefs.begin(NS, false); }

// The record's length straight from NVS: 0 only when there is no record.
// Preferences reports any lookup error as "no key", which would let a save
// replace a record that is there but can't be read; here an error reads as
// unreadable (SIZE_MAX).
size_t stored_length() {
  nvs_handle_t h;
  esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
  if (e == ESP_ERR_NVS_NOT_FOUND) return 0;
  if (e != ESP_OK) return SIZE_MAX;
  size_t n = 0;
  e = nvs_get_blob(h, "cfg", nullptr, &n);
  nvs_close(h);
  if (e == ESP_ERR_NVS_NOT_FOUND) return 0;
  return e == ESP_OK && n ? n : SIZE_MAX;
}

size_t store_read(uint8_t* buf) {
  if (!open_) return 0;
  size_t n = stored_length();
  if (n == 0) return 0;
  if (n > CFG_SIZE) {
    // Too long, or its length can't be read: invalid, never "absent", which
    // would allow a save over it. The zeroed buffer fails validation.
    memset(buf, 0, CFG_SIZE);
    return n == SIZE_MAX ? 1 : n;
  }
  if (prefs.getBytes("cfg", buf, n) != n) {
    // The record is there but couldn't be read: invalid (recovery), not
    // "absent".
    memset(buf, 0, CFG_SIZE);
    return n;
  }
  return n;
}

uint8_t store_write(const uint8_t* rec) {
  if (!open_ || prefs.putBytes("cfg", rec, CFG_SIZE) != CFG_SIZE) return E_WRITE;
  static uint8_t back[CFG_SIZE];
  if (prefs.getBytes("cfg", back, CFG_SIZE) != CFG_SIZE || memcmp(back, rec, CFG_SIZE)) return E_VERIFY;
  saved_crc = config_crc(rec);
  if (!old_keys_cleared) {
    for (const char* k : OLD_KEYS)
      if (prefs.isKey(k)) prefs.remove(k);
    old_keys_cleared = true;
  }
  return E_OK;
}

uint32_t store_saved_crc() { return saved_crc; }

void store_erase() {
  if (open_) prefs.remove("cfg");
}

bool store_service(Core& core, const Config& cfg, bool persist, uint32_t now) {
  bool flush = core.take_flush();
  if (!persist || !core.dirty()) return false;
  if (!flush) {
    // No writes while a key screen is live, and not until edits settle.
    if (core.keymap_live()) return false;
    if (now - core.last_edit() < IDLE_MS) return false;
    if (retrying && !due(now, retry_at)) return false;
  }
  static uint8_t rec[CFG_SIZE];
  config_encode(cfg, rec);
  if (store_write(rec) == E_OK) {
    core.clean();
    retrying = false;
    return true;
  }
  retrying = true;
  retry_at = now + RETRY_MS;
  return false;
}

}  // namespace mp
