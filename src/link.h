// The serial link: line reader, reply and log outboxes, session breaks.
// Pure: no Arduino headers. The CDC hardware sits behind CdcPort.
//
// Rules (see docs/protocol.md):
// - Input is read byte by byte into a 160-byte line buffer, stopping at a
//   complete line. A line is handed out only when no reply is pending, and
//   while it waits nothing more is read, so pipelined lines wait in the FIFO.
// - Replies take precedence over logs: a reply drops log lines that haven't
//   started; a log line already being sent is never cut.
// - Logs are queued whole or not at all, only while the host is connected.
// - A session break (DTR falling, or the caller reporting a USB break) clears
//   everything and purges the CDC FIFOs.
#pragma once

#include <stdint.h>

namespace mp {

struct CdcPort {
  virtual bool connected() = 0;  // DTR
  virtual uint32_t available() = 0;
  virtual uint32_t read(uint8_t* buf, uint32_t n) = 0;
  virtual uint32_t write_available() = 0;
  virtual uint32_t write(const uint8_t* buf, uint32_t n) = 0;
  virtual void flush() = 0;  // send what's queued now
  virtual void purge() = 0;  // drop both FIFOs
};

class Link {
 public:
  static constexpr uint16_t MAX_LINE = 160;   // bytes, including the newline
  static constexpr uint16_t READ_PER_POLL = 256;
  static constexpr uint16_t REPLY_CAP = 512;
  static constexpr uint16_t LOG_CAP = 1536;

  explicit Link(CdcPort& port) : port_(port) {}

  // Every loop: detect DTR edges. Returns true on a session break.
  bool watch();
  // Report a USB-level break (unmount, re-enumeration).
  void usb_break();

  // Read input (Proto::step calls it every loop in config mode).
  void poll();
  // A complete line, if one is ready and no reply is pending. The newline and
  // a trailing \r are removed. Returns nullptr otherwise. The pointer stays
  // valid until the next poll().
  const char* take_line();
  // True if the last discarded line was too long (cleared by reading it).
  bool take_overflow();

  // Queue a reply line (without newline). Drops unstarted logs to make room.
  void reply(const char* line);
  // Room for a reply line of this length (multi-line replies are paced).
  bool reply_room(uint16_t len) const { return reply_len_ + len + 1 <= REPLY_CAP; }
  // Start a fresh session (entering config mode): clear everything, purge
  // the FIFOs, and take the current DTR as the baseline without a break.
  void start();
  // Queue a log line (without the "# " prefix or newline) if connected and it
  // fits whole. Returns false if dropped.
  bool log(const char* line);
  // Room for a whole log line of this length right now (paced output).
  bool log_room(uint16_t len) const;

  // Send what fits. Call every loop.
  void service();

  bool reply_pending() const { return reply_len_ > 0; }
  // Self-test builds: replies keep queued log lines (the report must not be
  // cut short by a command sent while it drains).
  void keep_logs(bool on) { keep_logs_ = on; }
  // Anything left to send (a reply, or a log line).
  bool output_pending() const { return reply_len_ > 0 || log_len_ > 0; }
  // Drop log lines that haven't started (leaving config mode: a reply such
  // as EXIT's OK still goes out).
  void drop_logs() { log_len_ = log_inflight_; }
  bool connected() const { return connected_; }
  uint32_t stalls() const { return stalls_; }  // polls with reply bytes pending and no TX room
  void count_stalls(bool on) { counting_ = on; }
  void reset_stalls() { stalls_ = 0; }

 private:
  void reset();
  bool push(char* q, uint16_t cap, uint16_t& len, const char* a, const char* b);

  CdcPort& port_;
  bool connected_ = false;

  char line_[MAX_LINE + 1];
  uint16_t line_len_ = 0;
  bool line_ready_ = false;
  bool discarding_ = false;
  bool overflow_ = false;
  char out_line_[MAX_LINE + 1];

  char reply_[REPLY_CAP];
  uint16_t reply_len_ = 0;
  char log_[LOG_CAP];
  uint16_t log_len_ = 0;
  uint16_t log_inflight_ = 0;  // bytes left of a log line already started

  uint32_t stalls_ = 0;
  bool counting_ = false;
  bool keep_logs_ = false;
};

}  // namespace mp
