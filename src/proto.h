// The config protocol (v1), served in config and recovery modes. Pure.
// Commands and replies: docs/protocol.md.
#pragma once

#include <stdint.h>

#include "config.h"
#include "link.h"

namespace mp {

struct ProtoEnv {
  // What HELLO reports.
  const char* fw;
  uint32_t nonce;
  const char* cfg_status;  // "ok", "none", "invalid 4", "default crash", ...
  bool has_crc;            // a valid stored blob exists
  uint32_t stored_crc;
  bool recovery;
  bool selftest;  // TEST* commands allowed
  // Actions. commit gets a validated record and returns E_OK, E_WRITE or
  // E_VERIFY; on E_OK the app freezes the pad and restarts it.
  uint8_t (*commit)(const uint8_t* rec);
  void (*exit)();
  void (*test)(const char* what);  // "corrupt", "crash", "detach"
  // The record staging edits; the app sets it on entering config mode.
  uint8_t staging[CFG_SIZE];
};

class Proto {
 public:
  explicit Proto(ProtoEnv& env) : env_(env) {}
  // Call every loop in config and recovery mode: reads the link, then
  // handles at most one line.
  void step(Link& link);
  // A valid command has arrived since the last reset (the client is
  // "connected" for the cancel rules).
  bool got_valid() const { return got_valid_; }
  uint32_t last_valid_ms() const { return last_valid_; }
  void reset() {
    gen_ = NONE;
    got_valid_ = false;
  }
  void set_now(uint32_t now) { now_ = now; }

 private:
  void handle(char* line, Link& link);
  bool gen_line(char* out);
  void valid();

  ProtoEnv& env_;
  enum Gen : uint8_t { NONE, PINS, TOOLS } gen_ = NONE;
  uint8_t gen_i_ = 0, gen_j_ = 0;
  bool got_valid_ = false;
  uint32_t last_valid_ = 0, now_ = 0;
};

}  // namespace mp
