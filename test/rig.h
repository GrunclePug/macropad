// Drives a Core the way the main loop does, one millisecond per tick, from a
// config record.
#pragma once

#include "../src/core.h"
#include "../src/runtime.h"
#include "fake_port.h"

struct Rig {
  mp::Config cfg;
  mp::Runtime rt;
  mp::Input in;
  mp::Arbiter hid;
  mp::Rng rng;
  mp::Core core;
  FakePort port;
  uint32_t t = 1000;
  bool up = true;
  bool disturbed = false;
  uint8_t keys = 0, buttons = 0, enc = 0;

  explicit Rig(const mp::Config& c, bool start_up = true) : cfg(c) {
    up = start_up;
    mp::config_to_variant(cfg, rt);
    core.begin(rt.v, cfg, in, hid, rng);
    run(10);
  }
  void tick() {
    port.up = up;
    in.sample(keys, buttons, enc, t);
    core.link(up, disturbed);
    disturbed = false;
    mp::Event e;
    while (in.pop(e)) core.handle(e, t);
    if (core.take_config_request()) core.enter_config(false);
    if (core.take_cancel()) core.leave_config();
    core.update(t);
    hid.collect();
    port.drain(hid);
    t++;
  }
  void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) tick();
  }
  void tap_button(int b) {
    buttons |= 1u << b;
    run(60);
    buttons &= ~(1u << b);
    run(60);
  }
  void hold_button(int b) {
    buttons |= 1u << b;
    run(800);
    buttons &= ~(1u << b);
    run(60);
  }
  void tap_key(int k) {
    keys |= 1u << k;
    run(60);
    keys &= ~(1u << k);
    run(60);
  }
  // One encoder detent (4 transitions), clockwise or not.
  void turn(bool cw) {
    static const uint8_t CW[] = {2, 3, 1, 0}, CCW[] = {1, 3, 2, 0};
    for (uint8_t s : cw ? CW : CCW) {
      enc = s;
      run(2);
    }
  }
};
