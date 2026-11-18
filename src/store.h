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
size_t store_read(uint8_t* buf);
// Write an encoded record, read it back and compare. Returns E_OK, E_WRITE
// or E_VERIFY.
uint8_t store_write(const uint8_t* rec);
// The pad's own saves: 1 s after the last edit off key screens, and at once
// when a key screen is about to go live. Only while persist is on.
void store_service(Core& core, const Config& cfg, bool persist, uint32_t now);

}  // namespace mp
