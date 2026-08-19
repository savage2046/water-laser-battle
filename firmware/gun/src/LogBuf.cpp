#include "LogBuf.h"
#include <stdarg.h>

LogBuf logBuf;

void LogBuf::begin() {
  _head = _tail = _count = 0;
}

void LogBuf::log(const char *fmt, ...) {
  char tmp[ENTRY_LEN - 1];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  if (_count < CAPACITY) {
    snprintf(_buf[_head], ENTRY_LEN, "%s;", tmp);
    _head = (_head + 1) % CAPACITY;
    _count++;
  } else {
    // 满：覆盖最旧
    snprintf(_buf[_head], ENTRY_LEN, "%s;", tmp);
    _head = (_head + 1) % CAPACITY;
    _tail = _head;
  }
}

// 只读构造第 part 个分片（不消耗条目）；返回 true 表示该 part 有内容。
// total 每次调用都返回当前缓冲对应的总分片数（一致）。
bool LogBuf::nextPayload(char *out, size_t maxLen, uint16_t &part,
                         uint16_t &total) {
  if (_count == 0) return false;

  // 统计总分片数
  total = 1;
  {
    uint16_t idx = _tail;
    uint16_t remain = _count;
    size_t used = 0;
    while (remain > 0) {
      size_t l = strlen(_buf[idx]);
      if (used + l > maxLen) {
        total++;
        used = 0;
      }
      used += l;
      idx = (idx + 1) % CAPACITY;
      remain--;
    }
  }

  // 定位第 part 个分片起点并填充
  uint16_t idx = _tail;
  uint16_t remain = _count;
  uint16_t cur = 0;
  size_t used = 0;
  size_t usedThis = 0;
  out[0] = '\0';
  bool started = false;
  while (remain > 0) {
    size_t l = strlen(_buf[idx]);
    if (used + l > maxLen) {
      // 换片
      if (cur == part) break;
      cur++;
      used = 0;
      usedThis = 0;
      if (cur > part) break;
    }
    if (cur == part) {
      memcpy(out + usedThis, _buf[idx], l + 1);
      usedThis += l;
      started = true;
    }
    if (cur > part) break;
    used += l;
    idx = (idx + 1) % CAPACITY;
    remain--;
  }
  return started;
}

void LogBuf::clear() {
  _count = 0;
  _head = _tail = 0;
}

float LogBuf::usage() {
  return (float)_count / CAPACITY;
}
