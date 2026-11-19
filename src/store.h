// The config record in NVS: blob "cfg" in namespace "macropad" (the self-test
// build uses "macropad-test"). ESP32 only.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "core.h"

namespace mp {

void store_begin();
// Read the stored blob into buf (CFG_SIZE bytes). Returns its length, 0 if
// there is none.
// buf holds CFG_SIZE; returns the stored length, 0 only when there is none.
// A record that is there but can't be read returns nonzero with buf zeroed,
// so it fails validation (never reads as absent).
size_t store_read(uint8_t* buf);
// Write an encoded record, read it back and compare. Returns E_OK, E_WRITE
// or E_VERIFY.
uint8_t store_write(const uint8_t* rec);
// The crc of the record the last successful store_write() verified.
uint32_t store_saved_crc();
// Remove the record (self-test builds: TESTERASE).
void store_erase();
// The pad's own saves: 1 s after the last edit off key screens, and at once
// when a key screen is about to go live. Only while persist is on.
bool store_service(Core& core, const Config& cfg, bool persist, uint32_t now);  // true: it saved

}  // namespace mp
