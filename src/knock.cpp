#include "knock.h"

namespace mp {

bool Knock::feed(bool dtr, bool rts, uint32_t now) {
  bool knock_low = !dtr && rts;
  if (!holding_) {
    if (knock_low) {
      holding_ = true;
      since_ = now;
    }
    return false;
  }
  if (knock_low) return false;
  holding_ = false;
  uint32_t held = now - since_;
  return dtr && rts && held >= MIN_MS && held <= MAX_MS;
}

}  // namespace mp
