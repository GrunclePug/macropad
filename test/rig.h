// Drives a Core the way the main loop does, one millisecond per tick.
#pragma once

#include "../src/core.h"
#include "fake_port.h"

struct Rig {
  mp::Input in;
  mp::Arbiter hid;
  mp::Rng rng;
  mp::Core core;
  FakePort port;
  uint32_t t = 1000;
  bool up = true;
  bool disturbed = false;
  uint8_t keys = 0, buttons = 0;

  explicit Rig(const mp::Variant& v, bool start_up = true) {
    up = start_up;
    core.begin(v, in, hid, rng);
    run(10);
  }
  void tick() {
    port.up = up;
    in.sample(keys, buttons, 0, t);
    core.link(up, disturbed);
    disturbed = false;
    mp::Event e;
    while (in.pop(e)) core.handle(e, t);
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
};
