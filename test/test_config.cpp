#include <stdio.h>
#include <string.h>

#include <fstream>
#include <sstream>

#include "../src/boot.h"
#include "../src/config.h"
#include "../src/knock.h"
#include "../src/proto.h"
#include "check.h"
#include "vectors_src.h"

using namespace mp;

static std::string read_line(const char* path) {
  std::ifstream f(path);
  std::string s;
  std::getline(f, s);
  return s;
}

TEST(crc32_is_zlib) {
  const char* s = "123456789";
  CHECK(crc32((const uint8_t*)s, 9) == 0xCBF43926u);
}

TEST(records_round_trip_and_validate) {
  Config cs[3];
  config_default(cs[0]);
  vec::config_full(cs[1]);
  config_deck(cs[2]);
  for (Config& c : cs) {
    uint8_t a[CFG_SIZE], b[CFG_SIZE];
    config_encode(c, a);
    CfgError e = config_validate(a, CFG_SIZE);
    CHECK(e.code == E_OK);
    if (e.code) printf("  validate: %s at %u\n", err_name(e.code), e.offset);
    Config d;
    config_decode(a, d);
    config_encode(d, b);
    CHECK(memcmp(a, b, CFG_SIZE) == 0);
  }
  Config st;
  config_selftest(st);
  uint8_t r[CFG_SIZE];
  config_encode(st, r);
  CHECK(config_validate(r, CFG_SIZE).code == E_OK);
  // Exactly CFG_SIZE: a longer buffer is refused, not truncated.
  {
    static uint8_t big[CFG_SIZE + 1];
    Config c;
    config_default(c);
    config_encode(c, big);
    CHECK(config_validate(big, CFG_SIZE).code == E_OK);
    CfgError e = config_validate(big, CFG_SIZE + 1);
    CHECK(e.code == E_LENGTH && e.offset == 6);
  }
}

TEST(vectors_match_the_constants) {
  Config c;
  config_default(c);
  CHECK(read_line("test/vectors/default.hex") == vec::hex(vec::enc(c)));
  vec::config_full(c);
  CHECK(read_line("test/vectors/full.hex") == vec::hex(vec::enc(c)));
  config_deck(c);
  CHECK(read_line("test/vectors/deck.hex") == vec::hex(vec::enc(c)));
}

TEST(invalid_records_give_their_code_and_offset) {
  for (const vec::Invalid& i : vec::invalid_cases()) {
    CfgError e = config_validate(i.rec.data(), i.rec.size());
    bool ok = e.code == i.code && e.offset == i.offset;
    CHECK(ok);
    if (!ok)
      printf("  %s: got %s at %u, want %s at %u\n", i.name.c_str(), err_name(e.code), e.offset, err_name(i.code),
             i.offset);
  }
  // The file on disk is the same table.
  std::ifstream f("test/vectors/invalid.tsv");
  std::string line;
  size_t n = 0;
  while (std::getline(f, line))
    if (!line.empty() && line[0] != '#') n++;
  CHECK(n == vec::invalid_cases().size());
}

TEST(boot_rows_and_overlaps) {
  BootDecision d;
  d = boot_decide({BlobState::Valid, 0, false, false});
  CHECK(d.mode == BootMode::Normal && d.use_blob && d.persist && d.status == CfgStatus::Ok);
  d = boot_decide({BlobState::None, 0, false, false});
  CHECK(d.mode == BootMode::Normal && !d.use_blob && d.persist && d.status == CfgStatus::None);
  d = boot_decide({BlobState::Invalid, E_CRC, false, false});
  CHECK(d.mode == BootMode::Recovery && !d.use_blob && !d.persist && d.invalid_code == E_CRC);
  d = boot_decide({BlobState::Invalid, E_CRC, true, false});  // crash beats invalid
  CHECK(d.mode == BootMode::Normal && !d.persist && d.status == CfgStatus::DefaultCrash);
  d = boot_decide({BlobState::Invalid, E_CRC, true, true});  // safe beats both
  CHECK(d.status == CfgStatus::DefaultSafe && !d.persist);
  d = boot_decide({BlobState::None, 0, true, false});
  CHECK(d.status == CfgStatus::DefaultCrash && !d.persist);
  d = boot_decide({BlobState::Valid, 0, true, false});
  CHECK(!d.use_blob && !d.persist);
}

TEST(knock_pattern) {
  Knock k;
  uint32_t t = 0;
  // A plain open (DTR and RTS together) never knocks.
  CHECK(!k.feed(true, true, t));
  // DTR low with RTS high for 500 ms, then DTR up: a knock.
  CHECK(!k.feed(false, true, t += 10));
  CHECK(!k.feed(false, true, t += 500));
  CHECK(k.feed(true, true, t += 1));
  // Too short.
  k.feed(false, true, t += 10);
  CHECK(!k.feed(true, true, t += 100));
  // Too long.
  k.feed(false, true, t += 10);
  CHECK(!k.feed(true, true, t += 1500));
  // Ends with RTS low: not a knock.
  k.feed(false, true, t += 10);
  CHECK(!k.feed(true, false, t += 500));
  // DTR alone toggling (RTS low): never.
  k.feed(false, false, t += 10);
  CHECK(!k.feed(true, false, t += 500));
  // Windows opens with both lines low: (0,0), then RTS up, then DTR up.
  k.reset();
  CHECK(!k.feed(false, false, t += 10));
  CHECK(!k.feed(false, true, t += 10));
  CHECK(!k.feed(false, true, t += 500));
  CHECK(k.feed(true, true, t += 1));
  // A close (both low) in the middle of the low phase: not a knock.
  CHECK(!k.feed(false, true, t += 10));
  CHECK(!k.feed(false, false, t += 400));
  CHECK(!k.feed(true, true, t += 100));
  // A low phase already under way at the first sample counts from there.
  k.reset();
  CHECK(!k.feed(false, true, t += 10));
  CHECK(k.feed(true, true, t += 300));
}

// --- Protocol ---

struct ProtoCdc : CdcPort {
  bool dtr = true;
  std::string in, out;
  bool connected() override { return dtr; }
  uint32_t available() override { return (uint32_t)in.size(); }
  uint32_t read(uint8_t* b, uint32_t n) override {
    uint32_t k = n < in.size() ? n : (uint32_t)in.size();
    memcpy(b, in.data(), k);
    in.erase(0, k);
    return k;
  }
  uint32_t write_available() override { return 63; }
  uint32_t write(const uint8_t* b, uint32_t n) override {
    out.append((const char*)b, n);
    return n;
  }
  void flush() override {}
  void purge() override { in.clear(); }
};

static int commits = 0, exits = 0;
static uint8_t commit_ok(const uint8_t*) {
  commits++;
  return E_OK;
}
static void on_exit() { exits++; }

struct ProtoRig {
  ProtoCdc cdc;
  Link link{cdc};
  ProtoEnv env{};
  Proto proto{env};
  ProtoRig() {
    env.fw = "0.3.0";
    env.nonce = 0xdeadbeef;
    env.cfg_status = "ok";
    env.has_crc = true;
    env.stored_crc = 0x1234;
    env.commit = commit_ok;
    env.exit = on_exit;
    Config c;
    config_default(c);
    config_encode(c, env.staging);
    link.start();
  }
  std::string ask(const std::string& cmd) {
    cdc.out.clear();
    cdc.in += cmd + "\n";
    for (int i = 0; i < 200; i++) {
      proto.step(link);  // reads the link itself, as on the pad
      link.service();
    }
    return cdc.out;
  }
};

TEST(proto_hello_get_put_check_commit) {
  ProtoRig r;
  std::string h = r.ask("HELLO");
  CHECK(h == "OK\tmacropad\t0.3.0\tproto 1\tmode config\tcfg ok\tcrc 00001234\tboot deadbeef\n");
  r.env.sn = "DCDA0C20EF54";
  CHECK(r.ask("HELLO") == "OK\tmacropad\t0.3.0\tproto 1\tmode config\tcfg ok\tcrc 00001234\tboot deadbeef\tsn DCDA0C20EF54\n");
  r.env.sn = nullptr;
  CHECK(r.proto.got_valid());
  std::string g = r.ask("GET\t0\t4");
  CHECK(g == "DATA\t4d504331\nOK\n");  // "MPC1"
  CHECK(r.ask("GET\t1352\t4").find("ERR\t103\t0\tE_RANGE") == 0);
  CHECK(r.ask("PUT\t16\t03").find("OK") == 0);  // key count 3: too few controls
  CHECK(r.ask("PUT\t20\tff").find("OK") == 0);  // and key slot 3 unused
  CHECK(r.ask("CHECK").find("ERR\t4\t12\tE_CRC") == 0);  // CRC no longer matches
  // Fix the CRC and check again: now the controls rule fires.
  uint8_t* s = r.env.staging;
  memset(s + 12, 0, 4);
  uint32_t c = crc32(s, CFG_SIZE);
  for (int i = 0; i < 4; i++) s[12 + i] = (uint8_t)(c >> (8 * i));
  CHECK(r.ask("CHECK").find("ERR\t38\t16\tE_CONTROLS") == 0);
  // Back to the default and commit.
  Config d;
  config_default(d);
  config_encode(d, r.env.staging);
  CHECK(r.ask("CHECK") == "OK\n");
  char want[24];
  snprintf(want, sizeof want, "OK\t%08lx\n", (unsigned long)config_crc(r.env.staging));
  CHECK(r.ask("COMMIT") == want && commits == 1);
}

TEST(proto_errors_and_pacing) {
  ProtoRig r;
  CHECK(r.ask("NOPE").find("ERR\t101\t0\tE_MODE") == 0);
  CHECK(r.ask("PUT\t0\tzz").find("ERR\t104") == 0);
  CHECK(r.ask("PUT\t0\t123").find("ERR\t104") == 0);
  CHECK(r.ask("GET\tx\t1").find("ERR\t102") == 0);
  // Offsets that would overflow off + len, and the last valid ones.
  CHECK(r.ask("GET\t9223372036854775807\t64").find("ERR\t103") == 0);
  CHECK(r.ask("GET\t2147483647\t64").find("ERR\t103") == 0);
  CHECK(r.ask("GET\t1354\t2").find("ERR\t103") == 0);
  CHECK(r.ask("GET\t1354\t1").find("DATA\t") == 0);
  CHECK(r.ask("PUT\t9223372036854775807\tabcd").find("ERR\t103") == 0);
  CHECK(r.ask("PUT\t2147483647\tabcd").find("ERR\t103") == 0);
  CHECK(r.ask("PUT\t1354\tabcd").find("ERR\t103") == 0);
  CHECK(r.ask(std::string(200, 'A')).find("ERR\t100\t0\tE_LINE") == 0);
  std::string p = r.ask("PINS");
  CHECK(p.find("PIN\t0\tD2\n") == 0 && p.find("PIN\t15\tA7\n") != std::string::npos && p.substr(p.size() - 3) == "OK\n");
  std::string t = r.ask("TOOLS");
  CHECK(t.find("TOOL\t1\tclick\tAutoclicker\n") == 0);
  CHECK(t.find("PARAM\t1\t2\tbtn\tButton\tchoice\t0\t2\t1\t\tLeft,Right,Middle\n") != std::string::npos);
  CHECK(t.find("PARAM\t3\t0\tkey\tKey\tkey\t4\t127\t1\t\t\n") != std::string::npos);
  CHECK(t.substr(t.size() - 3) == "OK\n");
  CHECK(r.ask("EXIT") == "OK\n" && exits == 1);
  r.env.recovery = true;
  CHECK(r.ask("EXIT").find("ERR\t101") == 0);
  // Self-test commands that need normal mode afterwards: not in recovery.
  r.env.selftest = true;
  CHECK(r.ask("TESTRUN").find("ERR\t101") == 0 && r.ask("TESTTAP").find("ERR\t101") == 0 &&
        r.ask("TESTACK").find("ERR\t101") == 0);
}

void run_config_tests() {
  RUN(crc32_is_zlib);
  RUN(records_round_trip_and_validate);
  RUN(vectors_match_the_constants);
  RUN(invalid_records_give_their_code_and_offset);
  RUN(boot_rows_and_overlaps);
  RUN(knock_pattern);
  RUN(proto_hello_get_put_check_commit);
  RUN(proto_errors_and_pacing);
}
