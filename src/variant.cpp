// Picks the variant's tables and pins at build time.
#include "variant.h"

#if defined(MACROPAD_VARIANT_PAD)
#include "variants/pad.h"
#include "variants/pad_pins.h"
#elif defined(MACROPAD_VARIANT_DECK)
#include "variants/deck.h"
#include "variants/deck_pins.h"
#else
#error "No variant selected: build with make VARIANT=<name>"
#endif
