#include "link.h"

#include <string.h>

namespace mp {

void Link::reset() {
  line_len_ = 0;
  line_ready_ = false;
  discarding_ = false;
  overflow_ = false;
  reply_len_ = 0;
  log_len_ = 0;
  log_inflight_ = 0;
  port_.purge();
}

bool Link::watch() {
  bool c = port_.connected();
  bool broke = connected_ && !c;
  connected_ = c;
  if (broke) reset();
  return broke;
}

void Link::start() {
  reset();
  connected_ = port_.connected();
}

void Link::usb_break() {
  connected_ = false;
  reset();
}

void Link::poll() {
  if (line_ready_) return;  // a line waits to be dispatched
  uint16_t budget = READ_PER_POLL;
  while (budget-- && port_.available()) {
    uint8_t c;
    if (!port_.read(&c, 1)) break;
    if (discarding_) {
      if (c == '\n') discarding_ = false;
      continue;
    }
    if (c == '\n') {
      if (line_len_ && line_[line_len_ - 1] == '\r') line_len_--;
      line_[line_len_] = 0;
      line_ready_ = true;
      return;
    }
    if (line_len_ >= MAX_LINE - 1) {  // no room for this byte plus the newline
      line_len_ = 0;
      discarding_ = true;
      overflow_ = true;
      continue;
    }
    line_[line_len_++] = (char)c;
  }
}

const char* Link::take_line() {
  if (!line_ready_ || reply_pending()) return nullptr;
  memcpy(out_line_, line_, line_len_ + 1);
  line_ready_ = false;
  line_len_ = 0;
  return out_line_;
}

bool Link::take_overflow() {
  bool o = overflow_;
  overflow_ = false;
  return o;
}

bool Link::push(char* q, uint16_t cap, uint16_t& len, const char* a, const char* b) {
  size_t la = strlen(a), lb = strlen(b);
  if (len + la + lb + 1 > cap) return false;
  memcpy(q + len, a, la);
  memcpy(q + len + la, b, lb);
  q[len + la + lb] = '\n';
  len += la + lb + 1;
  return true;
}

void Link::reply(const char* line) {
  if (!push(reply_, REPLY_CAP, reply_len_, line, "")) {
    // Replies are at most MAX_LINE and generated one request at a time, so
    // this only happens on a bug: keep the newest reply rather than none.
    reply_len_ = 0;
    push(reply_, REPLY_CAP, reply_len_, line, "");
  }
  // Drop log lines that haven't started; keep the one being sent.
  if (!keep_logs_) log_len_ = log_inflight_;
}

bool Link::log_room(uint16_t len) const {
  return connected_ && log_len_ + len + 3 <= LOG_CAP;
}

bool Link::log(const char* line) {
  if (!connected_) return false;
  return push(log_, LOG_CAP, log_len_, "# ", line);
}

void Link::service() {
  if (!reply_len_ && !log_len_) return;  // nothing to send: no CDC calls at all
  uint32_t room = port_.write_available();
  if (counting_ && reply_len_ && room == 0) stalls_++;
  // Finish a log line in flight, then replies, then start the next log line.
  bool sent = false;
  while (room) {
    char* q;
    uint16_t* len;
    uint16_t n;
    bool is_log;
    if (log_inflight_) {
      q = log_;
      len = &log_len_;
      n = log_inflight_;
      is_log = true;
    } else if (reply_len_) {
      q = reply_;
      len = &reply_len_;
      n = reply_len_;
      is_log = false;
    } else if (log_len_) {
      const char* nl = (const char*)memchr(log_, '\n', log_len_);
      log_inflight_ = nl ? (uint16_t)(nl - log_ + 1) : log_len_;
      continue;
    } else {
      break;
    }
    if (n > room) n = (uint16_t)room;
    uint32_t w = port_.write((const uint8_t*)q, n);
    if (!w) break;
    memmove(q, q + w, *len - w);
    *len -= w;
    if (is_log) log_inflight_ -= w;
    room -= w;
    sent = true;
  }
  if (sent) port_.flush();
}

}  // namespace mp
