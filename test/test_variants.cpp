// Every variant's tables are consistent and its NVS keys fit.
#include "../src/settings.h"
#include "../src/variant.h"
#include "check.h"

using namespace mp;

const Variant& deck_variant();  // test_deck.cpp

static void check_variant(const Variant& v) {
  CHECK(v.keys >= 1 && v.keys <= MAX_KEYS);
  CHECK(v.nbuttons <= MAX_BUTTONS && v.nentries >= 1);
  for (uint8_t e = 0; e < v.nentries; e++)
    CHECK(v.entries[e].tool ? v.entries[e].index < v.ntools : v.entries[e].index < v.nkeymaps);
  for (uint8_t k = 0; k < v.nkeymaps; k++)
    for (uint8_t i = 0; i < v.keys; i++) {
      const Binding& b = v.keymaps[k].keys[i];
      if (b.kind == Binding::KEY) CHECK(b.usage < 0x80);
      if (b.kind == Binding::CONSUMER) CHECK(b.usage < CC_COUNT);
      if (b.kind == Binding::TOGGLE_TOOL) CHECK(b.usage < v.ntools);
    }
  char key[16];
  for (uint8_t t = 0; t < v.ntools; t++) {
    CHECK(strlen(v.tools[t]->id) <= 6);
    size_t n;
    const Param* p = v.tools[t]->params(n);
    CHECK(n <= 16);
    for (size_t i = 0; i < n; i++) {
      CHECK(settings_key(key, v.tools[t]->id, p[i].id));
      CHECK(*p[i].value >= p[i].min && *p[i].value <= p[i].max);
      for (size_t j = 0; j < i; j++) CHECK(strcmp(p[i].id, p[j].id) != 0);
    }
  }
}

TEST(pad_tables) { check_variant(VARIANT); }
TEST(deck_tables) { check_variant(deck_variant()); }

TEST(settings_key_limit) {
  char key[16];
  CHECK(settings_key(key, "click", "min") && strcmp(key, "click.min") == 0);
  CHECK(settings_key(key, "abcdef", "ghijklmn"));   // 15 bytes
  CHECK(!settings_key(key, "abcdef", "ghijklmno"));  // 16
}

void run_variant_tests() {
  RUN(pad_tables);
  RUN(deck_tables);
  RUN(settings_key_limit);
}
