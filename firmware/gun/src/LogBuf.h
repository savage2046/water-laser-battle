#pragma once
#include <Arduino.h>

// 设备日志：环形缓冲累积紧凑条目，在合适时机批量上传（470M L 帧分片）。
// 条目格式："<code>,<tSec>,<p1>[,<p2>[,<p3>]]"，以 ';' 分隔拼接进分片 payload。
class LogBuf {
 public:
  void begin();

  // 记录一条日志（printf 风格，自动加时间戳前缀交给调用方自行拼）
  void log(const char *fmt, ...);

  // 是否有待传日志
  bool pending() { return _count > 0; }
  uint16_t count() { return _count; }

  // 构造一个分片帧（part 从 0 起），返回 true 表示还有更多分片
  // frame 输出不含帧类型前缀，调用方自行 sendFmt("L%d,%u,%u,%u,%s", ...)
  bool nextPayload(char *out, size_t maxLen, uint16_t &part, uint16_t &total);

  // 批量上传完成（服务器已收全/放弃），清空缓冲
  void clear() { _count = 0; _head = 0; _tail = 0; }

  // 水位：已用/容量（0.0~1.0）
  float usage();

 private:
  static const uint16_t CAPACITY = 48;   // 条目数
  static const uint16_t ENTRY_LEN = 44;  // 单条最大长度（含 ';'）
  char _buf[CAPACITY][ENTRY_LEN];
  uint16_t _head = 0;   // 写位置
  uint16_t _tail = 0;   // 读位置
  uint16_t _count = 0;
};

extern LogBuf logBuf;
