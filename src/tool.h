// Tools: background automation with a table of settings. Pure: no Arduino
// headers.
//
// The core draws and edits a tool's parameters, saves them, and decides when
// it runs: start() on the edge to active (enabled, not killed, USB up), stop()
// on the edge back, tick() while active. Tools never see the killswitch.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hid.h"
#include "sched.h"

namespace mp {

struct Param {
  const char* id;     // short ASCII, part of the NVS key
  const char* label;
  int32_t* value;
  int32_t min, max, step;
  const char* unit;
  const char* const* names;  // optional: labels for a choice (value = index)
};

struct ToolCtx {
  Arbiter& hid;
  Rng& rng;
};

class Tool {
 public:
  Tool(const char* id, const char* name) : id(id), name(name) {}
  virtual ~Tool() = default;

  virtual const Param* params(size_t& n) {
    n = 0;
    return nullptr;
  }
  virtual void clamp() {}                         // fix rules across parameters
  virtual void start(ToolCtx&, uint32_t) {}  // became active; out is empty
  virtual void stop() {}                          // became inactive; out already cleared
  virtual void tick(ToolCtx&, uint32_t) {}   // while active; must not block

  const char* id;    // stable, at most 6 bytes
  const char* name;
  Output out;
};

}  // namespace mp
