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

}  // namespace mp
