// A stream deck: 8 keys, encoder for volume, no automation and no killswitch.
// Planned for a second pad (Windows: OBS and Discord hotkeys); the bindings are
// placeholders until it's built. Tables only. Pure: no Arduino headers.
#pragma once

#include "../core.h"

namespace mp {
namespace deck {

const Keymap KEYMAPS[] = {
    {"deck", "Deck",
     {bind_key(key::F13, "F13"), bind_key(key::F14, "F14"), bind_key(key::F15, "F15"),
      bind_key(key::F16, "F16"), bind_key(key::F17, "F17"), bind_key(key::F18, "F18"),
      bind_key(key::F19, "F19"), bind_key(key::F20, "F20")},
     bind_consumer(CC_VOL_UP, "Vol+"), bind_consumer(CC_VOL_DOWN, "Vol-"),
     bind_consumer(CC_MUTE, "Mute"), true},
};

const Entry ENTRIES[] = {{false, 0}};

}  // namespace deck

const Variant VARIANT = {
    "deck",
    8,
    true,
    4,
    deck::KEYMAPS,
    1,
    nullptr,
    0,
    deck::ENTRIES,
    1,
    {{Role::Bind, bind_key(key::F21, "F21")}, {Role::Nav, {}}, {Role::EncPush, {}}},
    3,
};

}  // namespace mp
