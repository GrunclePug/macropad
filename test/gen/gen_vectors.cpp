// Writes test/vectors/: default.hex, full.hex, deck.hex and invalid.tsv.
// Run with `make vectors`; the tests (C++ and Rust) check against them.
#include <stdio.h>

#include "../vectors_src.h"

using namespace vec;

static void write(const char* path, const std::string& text) {
  FILE* f = fopen(path, "w");
  if (!f) {
    perror(path);
    return;
  }
  fputs(text.c_str(), f);
  fclose(f);
}

int main() {
  Config c;
  config_default(c);
  write("test/vectors/default.hex", hex(enc(c)) + "\n");
  config_full(c);
  write("test/vectors/full.hex", hex(enc(c)) + "\n");
  config_deck(c);
  write("test/vectors/deck.hex", hex(enc(c)) + "\n");
  std::string tsv = "# name\thex\tcode\toffset\n";
  for (const Invalid& i : invalid_cases())
    tsv += i.name + "\t" + hex(i.rec) + "\t" + std::to_string(i.code) + "\t" + std::to_string(i.offset) + "\n";
  write("test/vectors/invalid.tsv", tsv);
  return 0;
}
