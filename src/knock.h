// The knock: how a host asks a pad in normal mode to enter config mode
// without sending any data. Pure.
//
// The host holds DTR low with RTS high for 300-1000 ms, then raises DTR (RTS
// still high). Ordinary opens raise DTR and RTS together, or DTR alone, so
// they don't match. Fed the raw line-state bits every loop.
#pragma once

#include <stdint.h>

namespace mp {

class Knock {
 public:
  static constexpr uint32_t MIN_MS = 300, MAX_MS = 1000;
  // Returns true once, on the edge that completes a knock.
  bool feed(bool dtr, bool rts, uint32_t now_ms);
  void reset() { holding_ = false; }

 private:
  bool holding_ = false;
  uint32_t since_ = 0;
};

}  // namespace mp
