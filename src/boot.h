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

// What one read of the stored record found. Exact: valid, and the record
// just written.
enum class Flash : uint8_t { None, Unreadable, Valid, Exact };
// Which crc HELLO reports: none, the record read from flash, or the staged
// record (pad-side edits that a failed flush couldn't store).
enum class CrcFrom : uint8_t { None, Stored, Staged };

struct FlashView {
  bool landed;  // the failed write is in flash after all
  CrcFrom crc;
  bool fault;  // flash fault: `cfg ok`, `crc none`, no pad-side saves
};

// After a write that failed (E_WRITE or E_VERIFY), from one read-back.
// `verified`: flash held a verified record before. `flush`: the entry flush
// of pad-side edits (a COMMIT otherwise).
FlashView after_failed_write(Flash back, bool verified, bool flush);
// At config entry, from the read that stages (nothing to flush, or the
// flush landed).
FlashView at_entry(Flash read, bool verified);

}  // namespace mp
