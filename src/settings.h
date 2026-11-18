// Tool settings in NVS, one key per parameter. ESP32 only.
#pragma once

#include <stdint.h>

#include "core.h"

namespace mp {

// Builds "<tool id>.<param id>". Returns false if it won't fit an NVS key (15
// bytes). Pure, so the tests can check every key.
bool settings_key(char* out, const char* tool, const char* param);

void settings_begin(const Variant& v);  // load saved values
void settings_service(Core& c, uint32_t now);
bool settings_read(const char* tool, const char* param, int32_t& value);  // self-test

}  // namespace mp
