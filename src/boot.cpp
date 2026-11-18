#include "boot.h"

namespace mp {

BootDecision boot_decide(const BootInput& in) {
  if (in.safe_held) return {BootMode::Normal, false, false, CfgStatus::DefaultSafe, 0};
  if (in.crash_reset) return {BootMode::Normal, false, false, CfgStatus::DefaultCrash, 0};
  switch (in.blob) {
    case BlobState::Invalid:
      return {BootMode::Recovery, false, false, CfgStatus::Invalid, in.invalid_code};
    case BlobState::None:
      return {BootMode::Normal, false, true, CfgStatus::None, 0};
    case BlobState::Valid:
    default:
      return {BootMode::Normal, true, true, CfgStatus::Ok, 0};
  }
}

FlashView after_failed_write(Flash back, bool verified, bool flush) {
  CrcFrom kept = flush ? CrcFrom::Staged : CrcFrom::Stored;
  switch (back) {
    case Flash::Exact:
      return {true, CrcFrom::Stored, false};
    case Flash::Unreadable:
      return {false, CrcFrom::None, true};
    case Flash::None:  // a verified record gone is a fault; else nothing landed
      if (verified) return {false, CrcFrom::None, true};
      return {false, flush ? CrcFrom::Staged : CrcFrom::None, false};
    case Flash::Valid:  // the old record: a flush keeps reporting its edits
    default:
      return {false, kept, false};
  }
}

FlashView at_entry(Flash read, bool verified) {
  if (read == Flash::Valid || read == Flash::Exact) return {false, CrcFrom::Stored, false};
  // Unreadable or gone: a fault if flash held a verified record; otherwise
  // (nothing ever stored, or a safe or crash boot on an invalid record) no
  // crc, and the boot row stands.
  return {false, CrcFrom::None, verified};
}

}  // namespace mp
