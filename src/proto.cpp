#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace mp {
namespace {

const char* const PIN_NAMES[PIN_CODES] = {"D2", "D3", "D4",  "D5", "D6", "D7", "D8", "D9",
                                         "D10", "D11", "D12", "A0", "A1", "A3", "A6", "A7"};

void err(Link& l, uint8_t code, uint16_t off, const char* text) {
  char r[Link::MAX_LINE];
  snprintf(r, sizeof r, "ERR\t%u\t%u\t%s: %s", code, off, err_name(code), text);
  l.reply(r);
}

// Split on tabs, at most `max` fields. Returns the count.
int split(char* s, char** f, int max) {
  int n = 0;
  f[n++] = s;
  for (char* p = s; *p && n < max; p++)
    if (*p == '\t') {
      *p = 0;
      f[n++] = p + 1;
    }
  return n;
}

bool number(const char* s, long& out) {
  if (!*s) return false;
  char* end;
  out = strtol(s, &end, 10);
  return *end == 0 && out >= 0;
}

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

void Proto::valid() {
  got_valid_ = true;
  last_valid_ = now_;
}

bool Proto::gen_line(char* out) {
  size_t cap = Link::MAX_LINE;
  if (gen_ == PINS) {
    if (gen_i_ < PIN_CODES) {
      snprintf(out, cap, "PIN\t%u\t%s", gen_i_, PIN_NAMES[gen_i_]);
      gen_i_++;
      return true;
    }
  } else if (gen_ == TOOLS) {
    while (gen_i_ < TOOL_KINDS) {
      const ToolSchema* t = tool_schema(gen_i_ + 1);
      if (gen_j_ == 0) {
        snprintf(out, cap, "TOOL\t%u\t%s\t%s", t->kind, t->id, t->name);
        gen_j_ = 1;
        return true;
      }
      if (gen_j_ <= t->nparams) {
        const ParamDef& d = t->params[gen_j_ - 1];
        char names[64] = "";
        if (d.kind == P_CHOICE)
          for (uint8_t i = 0; d.names[i]; i++) {
            if (i) strncat(names, ",", sizeof names - strlen(names) - 1);
            strncat(names, d.names[i], sizeof names - strlen(names) - 1);
          }
        const char* kind = d.kind == P_CHOICE ? "choice" : d.kind == P_KEY ? "key" : "number";
        snprintf(out, cap, "PARAM\t%u\t%u\t%s\t%s\t%s\t%ld\t%ld\t%ld\t%s\t%s", t->kind, gen_j_ - 1, d.id,
                 d.label, kind, (long)d.min, (long)d.max, (long)d.step, d.unit, names);
        gen_j_++;
        return true;
      }
      gen_i_++;
      gen_j_ = 0;
    }
  }
  gen_ = NONE;
  strcpy(out, "OK");
  return true;
}

void Proto::step(Link& link) {
  link.poll();
  if (gen_ != NONE) {
    char line[Link::MAX_LINE];
    while (gen_ != NONE) {
      // Peek the next line's size by generating into a buffer only when the
      // reply outbox can take a full line.
      if (!link.reply_room(Link::MAX_LINE)) return;
      gen_line(line);
      link.reply(line);
    }
    return;
  }
  if (link.take_overflow()) {
    err(link, E_LINE, 0, "line too long");
    return;
  }
  const char* in = link.take_line();
  if (!in) return;
  char buf[Link::MAX_LINE + 1];
  strncpy(buf, in, sizeof buf - 1);
  buf[sizeof buf - 1] = 0;
  handle(buf, link);
}

void Proto::handle(char* line, Link& link) {
  char* f[4];
  int n = split(line, f, 4);
  const char* cmd = f[0];

  if (!strcmp(cmd, "HELLO")) {
    valid();
    char r[Link::MAX_LINE];
    char crc[9] = "none";
    if (env_.has_crc) snprintf(crc, sizeof crc, "%08lx", (unsigned long)env_.stored_crc);
    snprintf(r, sizeof r, "OK\tmacropad\t%s\tproto 1\tmode %s\tcfg %s\tcrc %s\tboot %08lx", env_.fw,
             env_.recovery ? "recovery" : "config", env_.cfg_status, crc, (unsigned long)env_.nonce);
    link.reply(r);
    return;
  }
  if (!strcmp(cmd, "PINS")) {
    valid();
    gen_ = PINS;
    gen_i_ = 0;
    step(link);
    return;
  }
  if (!strcmp(cmd, "TOOLS")) {
    valid();
    gen_ = TOOLS;
    gen_i_ = gen_j_ = 0;
    step(link);
    return;
  }
  if (!strcmp(cmd, "GET")) {
    valid();
    long off, len;
    if (n != 3 || !number(f[1], off) || !number(f[2], len)) return err(link, E_ARGS, 0, "GET offset len");
    // No off + len: a huge off would overflow. number() rejects negatives.
    if (len < 1 || len > 64 || off > (long)CFG_SIZE - len) return err(link, E_RANGE, 0, "out of range");
    char r[Link::MAX_LINE] = "DATA\t";
    char* p = r + 5;
    for (long i = 0; i < len; i++, p += 2) sprintf(p, "%02x", env_.staging[off + i]);
    link.reply(r);
    link.reply("OK");
    return;
  }
  if (!strcmp(cmd, "PUT")) {
    valid();
    long off;
    if (n != 3 || !number(f[1], off)) return err(link, E_ARGS, 0, "PUT offset hex");
    size_t h = strlen(f[2]);
    if (h == 0 || h % 2 || h > 128) return err(link, E_HEX, 0, "hex payload");
    if (off > (long)CFG_SIZE - (long)(h / 2)) return err(link, E_RANGE, 0, "out of range");
    uint8_t bytes[64];
    for (size_t i = 0; i < h / 2; i++) {
      int a = hexval(f[2][2 * i]), b = hexval(f[2][2 * i + 1]);
      if (a < 0 || b < 0) return err(link, E_HEX, 0, "hex payload");
      bytes[i] = (uint8_t)(a << 4 | b);
    }
    memcpy(env_.staging + off, bytes, h / 2);
    link.reply("OK");
    return;
  }
  if (!strcmp(cmd, "CHECK") || !strcmp(cmd, "COMMIT")) {
    valid();
    CfgError e = config_validate(env_.staging, CFG_SIZE);
    if (e.code) return err(link, e.code, e.offset, "invalid config");
    if (!strcmp(cmd, "CHECK")) return link.reply("OK");
    uint8_t r = env_.commit(env_.staging);
    if (r != E_OK) return err(link, r, 0, r == E_WRITE ? "flash write failed" : "read-back differs");
    char ok[24];
    snprintf(ok, sizeof ok, "OK\t%08lx", (unsigned long)config_crc(env_.staging));
    link.reply(ok);
    return;
  }
  if (!strcmp(cmd, "EXIT") && !env_.recovery) {
    valid();
    link.reply("OK");
    env_.exit();
    return;
  }
  if (env_.selftest && !strncmp(cmd, "TEST", 4)) {
    valid();
    link.reply("OK");
    if (!strcmp(cmd, "TESTCORRUPT")) env_.test("corrupt");
    if (!strcmp(cmd, "TESTCRASH")) env_.test("crash");
    if (!strcmp(cmd, "TESTDETACH")) env_.test("detach");
    if (!strcmp(cmd, "TESTRUN")) env_.test("run");
    if (!strcmp(cmd, "TESTTAP")) env_.test("tap");
    if (!strcmp(cmd, "TESTACK")) env_.test("ack");
    if (!strcmp(cmd, "TESTERASE")) env_.test("erase");
    return;
  }
  err(link, E_MODE, 0, "unknown command");
}

}  // namespace mp
