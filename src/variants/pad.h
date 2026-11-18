// The user's own pad: 4 keys, killswitch and nav buttons, no encoder yet.
// Tables only. Pure: no Arduino headers. Include from exactly one file.
#pragma once

#include "../core.h"
#include "../tools/tools.h"

namespace mp {
namespace pad {

#ifdef MACROPAD_SELFTEST
// Self-test: harmless outputs, so the test can run with nobody watching.
// F13-F17 and mouse button 8 aren't bound in the user's session.
constexpr uint8_t OSU[4] = {key::F13, key::F14, key::F15, key::F16};
constexpr uint8_t TEST_BUTTON = 0x80;
constexpr int32_t JITTER_PX = 10;
#else
constexpr uint8_t OSU[4] = {key::D, key::F, key::J, key::K};
constexpr uint8_t TEST_BUTTON = 0;
constexpr int32_t JITTER_PX = 100;
#endif

enum : uint8_t { T_CLICK, T_JITTER, T_SPAM, T_HOLD, NTOOLS };

const KeySet SPAM_SETS[] = {
#ifdef MACROPAD_SELFTEST
    {"F17", 0, {key::F17}},
#endif
    {"Space", 0, {key::SPACE}},
    {"E", 0, {key::E}},
    {"F", 0, {key::F}},
    {"1", 0, {key::N1}},
};
const char* const SPAM_NAMES[] = {
#ifdef MACROPAD_SELFTEST
    "F17",
#endif
    "Space", "E", "F", "1",
};

Autoclicker clicker(82, 103, TEST_BUTTON);
Jitter jitter(JITTER_PX, 4000, 7000);
Keyspam spam(SPAM_SETS, SPAM_NAMES, sizeof SPAM_SETS / sizeof SPAM_SETS[0], 202, 305, 50);
Mousehold hold(TEST_BUTTON);
Tool* const TOOLS[NTOOLS] = {&clicker, &jitter, &spam, &hold};

const Keymap KEYMAPS[] = {
    {"osu", "osu!mania",
     {bind_key(OSU[0], "D"), bind_key(OSU[1], "F"), bind_key(OSU[2], "J"), bind_key(OSU[3], "K")},
     {}, {}, {}, false},
    {"deck", "Deck",
     {bind_key(key::F13, "F13"), bind_key(key::F14, "F14"), bind_tool(T_CLICK, "Clicker"),
      bind_tool(T_JITTER, "Jitter")},
     bind_consumer(CC_VOL_UP, "Vol+"), bind_consumer(CC_VOL_DOWN, "Vol-"),
     bind_consumer(CC_MUTE, "Mute"), true},
};

const Entry ENTRIES[] = {
    {false, 0}, {false, 1}, {true, T_CLICK}, {true, T_JITTER}, {true, T_SPAM}, {true, T_HOLD},
};

}  // namespace pad

const Variant VARIANT = {
    "pad",
    4,
    false,
    4,
    pad::KEYMAPS,
    sizeof pad::KEYMAPS / sizeof pad::KEYMAPS[0],
    pad::TOOLS,
    pad::NTOOLS,
    pad::ENTRIES,
    sizeof pad::ENTRIES / sizeof pad::ENTRIES[0],
    {{Role::Kill, {}}, {Role::Nav, {}}, {}},
    2,
};

}  // namespace mp
