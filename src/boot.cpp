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

namespace {

bool valid(Flash f) { return f == Flash::Valid || f == Flash::Exact; }

FlashView view(bool landed, CrcFrom crc, bool fault, Flash f, FlashState st) {
  if (fault) crc = CrcFrom::None;
  return {landed, crc, fault, {st.verified || valid(f), st.faulted || fault}};
}

}  // namespace

FlashView after_failed_write(Flash back, FlashState st, bool flush) {
  switch (back) {
    case Flash::Exact:  // landed: a COMMIT restarts, a flush counts as saved
      return view(true, CrcFrom::Stored, st.faulted, back, st);
    case Flash::Unreadable:
      return view(false, CrcFrom::None, true, back, st);
    case Flash::None:  // a verified record gone is a fault; else nothing landed
      return view(false, flush ? CrcFrom::Staged : CrcFrom::None, st.verified || st.faulted, back, st);
    case Flash::Valid:  // the old record: a flush keeps reporting its edits
    default:
      return view(false, flush ? CrcFrom::Staged : CrcFrom::Stored, st.faulted, back, st);
  }
}

FlashView at_entry(Flash read, FlashState st) {
  if (valid(read)) return view(false, CrcFrom::Stored, st.faulted, read, st);
  // Unreadable or gone: a fault if flash held a verified record; otherwise
  // (nothing ever stored, or a safe or crash boot on an invalid record) no
  // crc, and the boot row stands.
  return view(false, CrcFrom::None, st.verified || st.faulted, read, st);
}

}  // namespace mp
