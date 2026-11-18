// The deck variant, compiled in its own file: without variant.h its VARIANT
// stays local here and doesn't clash with the pad's.
#include "../src/variants/deck.h"

const mp::Variant& deck_variant() { return mp::VARIANT; }
