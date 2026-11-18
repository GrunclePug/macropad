#include <string.h>

#include <string>

#include "../src/link.h"
#include "check.h"

using namespace mp;

struct FakeCdc : CdcPort {
  bool dtr = true;
  std::string in;           // bytes from the host
  std::string out;          // bytes the device sent
  uint32_t tx_room = 64;    // free TX FIFO space
  int purges = 0;
  bool connected() override { return dtr; }
  uint32_t available() override { return (uint32_t)in.size(); }
  uint32_t read(uint8_t* b, uint32_t n) override {
    uint32_t k = n < in.size() ? n : (uint32_t)in.size();
    memcpy(b, in.data(), k);
    in.erase(0, k);
    return k;
  }
  uint32_t write_available() override { return tx_room; }
  uint32_t write(const uint8_t* b, uint32_t n) override {
    uint32_t k = n < tx_room ? n : tx_room;
    out.append((const char*)b, k);
    tx_room -= k;
    return k;
  }
  void flush() override {}
  void purge() override {
    purges++;
    in.clear();
  }
  void drain() { tx_room = 64; }  // the host read the FIFO
};

static std::string run(Link& l, FakeCdc& c, int loops) {
  for (int i = 0; i < loops; i++) {
    l.service();
    c.drain();
  }
  return c.out;
}

TEST(link_reads_lines_and_strips_cr) {
  FakeCdc c;
  Link l(c);
  l.watch();
  c.in = "HELLO\r\nCONFIG\n";
  l.poll();
  const char* s = l.take_line();
  CHECK(s && strcmp(s, "HELLO") == 0);
  l.poll();
  s = l.take_line();
  CHECK(s && strcmp(s, "CONFIG") == 0);
}

TEST(link_holds_next_line_while_reply_pending) {
  FakeCdc c;
  Link l(c);
  l.watch();
  c.in = "A\nB\n";
  l.poll();
  CHECK(l.take_line());
  l.reply("a reply that is waiting");
  l.poll();
  CHECK(l.take_line() == nullptr);  // reply not yet written
  CHECK(c.in.empty());              // B is assembled and waits; nothing more is read
  run(l, c, 2);
  l.poll();
  const char* s = l.take_line();
  CHECK(s && strcmp(s, "B") == 0);
}

TEST(link_discards_long_lines) {
  FakeCdc c;
  Link l(c);
  l.watch();
  c.in = std::string(300, 'x') + "\nOK\n";
  for (int i = 0; i < 4 && !l.take_overflow(); i++) l.poll();
  l.poll();
  const char* s = l.take_line();
  CHECK(s && strcmp(s, "OK") == 0);
}

TEST(link_reply_drops_unstarted_logs_but_not_inflight) {
  FakeCdc c;
  Link l(c);
  l.watch();
  c.tx_room = 10;
  l.log("first log line that is long");
  l.log("second log line");
  l.service();  // starts the first log line: 10 bytes go out
  l.reply("OK\tdone");
  run(l, c, 10);
  // The first log line completes whole, then the reply; the second log is gone.
  CHECK(c.out == "# first log line that is long\nOK\tdone\n");
}

TEST(link_logs_only_while_connected) {
  FakeCdc c;
  Link l(c);
  c.dtr = false;
  l.watch();
  CHECK(!l.log("nobody listening"));
  c.dtr = true;
  l.watch();
  CHECK(l.log("hello"));
}

TEST(link_session_break_purges) {
  FakeCdc c;
  Link l(c);
  l.watch();
  c.in = "PART";
  l.poll();
  l.reply("pending");
  c.tx_room = 0;
  l.service();
  c.dtr = false;
  CHECK(l.watch());
  CHECK(!l.reply_pending() && c.purges == 1);
  c.dtr = true;
  l.watch();
  c.in = "X\n";
  l.poll();
  const char* s = l.take_line();
  CHECK(s && strcmp(s, "X") == 0);  // no leftover "PART"
}

TEST(link_counts_stalls_only_when_asked) {
  FakeCdc c;
  Link l(c);
  l.watch();
  l.reply("blocked");
  c.tx_room = 0;
  l.service();
  CHECK(l.stalls() == 0);
  l.count_stalls(true);
  l.service();
  l.service();
  CHECK(l.stalls() == 2);
}

// Leaving config mode: a queued reply (EXIT's OK) still goes out, unstarted
// logs don't, and nothing is pending afterwards.
TEST(link_leaving_keeps_the_reply) {
  FakeCdc c;
  Link l(c);
  l.watch();
  l.log("one");
  l.log("two");
  l.reply("OK");
  l.log("three");
  l.drop_logs();
  CHECK(l.output_pending());
  CHECK(run(l, c, 4) == "OK\n");
  CHECK(!l.output_pending());
}

void run_link_tests() {
  RUN(link_reads_lines_and_strips_cr);
  RUN(link_holds_next_line_while_reply_pending);
  RUN(link_discards_long_lines);
  RUN(link_reply_drops_unstarted_logs_but_not_inflight);
  RUN(link_logs_only_while_connected);
  RUN(link_session_break_purges);
  RUN(link_counts_stalls_only_when_asked);
  RUN(link_leaving_keeps_the_reply);
}
