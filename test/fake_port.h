// A fake USB endpoint that records submitted reports.
#pragma once

#include <string.h>

#include <vector>

#include "../src/hid.h"

struct Report {
  uint8_t id;
  uint8_t data[17];
  uint8_t len;
};

struct FakePort : mp::UsbPort {
  bool up = true;
  bool busy = false;
  std::vector<Report> sent;
  bool ready() override { return up && !busy; }
  bool submit(uint8_t id, const uint8_t* d, uint8_t len) override {
    Report r{id, {}, len};
    memcpy(r.data, d, len);
    sent.push_back(r);
    return true;
  }
  // Drain everything the arbiter has queued.
  void drain(mp::Arbiter& a) {
    while (a.service(*this)) {
    }
  }
};
