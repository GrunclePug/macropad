// Tools: background automation with a schema of settings. Pure: no Arduino
// headers.
//
// The core decides when a tool runs: start() on the edge to active (enabled,
// not killed, not in config mode, USB up), stop() on the edge back, tick()
// while active. Tools never see the killswitch. A tool's settings live in its
// slot of the running config, bound with bind(); the order is its schema's.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hid.h"
#include "sched.h"
#include "tools/schema.h"

namespace mp {

struct ToolCtx {
  Arbiter& hid;
  Rng& rng;
};

class Tool {
 public:
  explicit Tool(uint8_t kind) : kind(kind) {}
  virtual ~Tool() = default;

  void bind(int32_t* slot) { p_ = slot; }
  int32_t* slot() const { return p_; }
  const ToolSchema& schema() const { return *tool_schema(kind); }

  virtual void start(ToolCtx&, uint32_t) {}  // became active; out is empty
  virtual void stop() {}                     // became inactive; out already cleared
  virtual void tick(ToolCtx&, uint32_t) {}   // while active; must not block

  const uint8_t kind;
  Output out;

 protected:
  int32_t* p_ = nullptr;
};

}  // namespace mp
