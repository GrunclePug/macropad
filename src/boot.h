// The boot decision: which config runs, in which mode, and whether the pad
// may save its own edits. Pure; the first matching row wins (docs/protocol.md).
#pragma once

#include <stdint.h>

namespace mp {

enum class BlobState : uint8_t { None, Valid, Invalid };
enum class BootMode : uint8_t { Normal, Recovery };
enum class CfgStatus : uint8_t { Ok, None, Invalid, DefaultCrash, DefaultSafe };

struct BootInput {
  BlobState blob;
  uint8_t invalid_code;  // when Invalid
  bool crash_reset;      // panic or watchdog reset
  bool safe_held;        // a safe-boot pin held for 1 s at power-up
};

struct BootDecision {
  BootMode mode;
  bool use_blob;  // run the stored blob; otherwise the compiled default
  bool persist;   // the pad may save its own edits
  CfgStatus status;
  uint8_t invalid_code;
};

BootDecision boot_decide(const BootInput& in);

}  // namespace mp
