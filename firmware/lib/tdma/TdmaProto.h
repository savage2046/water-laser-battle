#pragma once
#include <Arduino.h>

// ============================================================
// TDMA 10 字节定长帧协议（T1，见 docs/protocol-tdma.md）
//   | devIdx(1B) | type+flags(1B) | seq(2B BE) | payload(5B) | crc8(1B) |
//   type  = byte[1] 高 5 位（0-31）
//   flags = byte[1] 低 3 位
//   crc8  = CRC-8/ATM（poly 0x07），覆盖 byte[0..8]
// ============================================================

struct TdmaFrame {
  uint8_t devIdx;        // 0-199 设备；0xFF=广播/信标
  uint8_t type;          // 0-31
  uint8_t flags;         // 0-7
  uint16_t seq;          // 16 位自增（上行去重用；下行恒 0）
  uint8_t payload[5];    // 5 字节事件数据
  uint8_t crc;           // 校验（解码时验证）
};

// ===== 帧类型（type 高 5 位）=====
enum {
  TF_BEACON   = 0x00,  // 信标（网关→全体，超帧起始）
  TF_JOIN     = 0x01,  // J 注册（上行）：payload=5B deviceId ASCII（如 "G0005"）
  TF_HIT      = 0x02,  // H 被打中（上行）：shooter,weapon,shotSeq,channel,hp
  TF_DEATH    = 0x03,  // D 阵亡（上行）：killer,shotSeq,channel,energy
  TF_FIRE     = 0x04,  // F 开火（上行）：空
  TF_HB       = 0x05,  // B 心跳（上行）：空
  TF_CAPTURE  = 0x06,  // C 夺旗（上行）：空
  TF_POS      = 0x07,  // P 位置（上行，3 分片）：见 protocol-tdma.md
  TF_TAG      = 0x08,  // T 头盔命中（上行）：shooter,weapon,shotSeq,channel
  TF_LOG      = 0x09,  // L 日志分片（上行）：part,total,data[3]
  TF_PAIR     = 0x0A,  // A 配对（上行）：空
  TF_WELCOME  = 0x0B,  // W 欢迎/规则（下行，3 分片）
  TF_START    = 0x0C,  // S 对局开始（下行，可广播）：mode(0 tdm 1 ffa 2 ctf)
  TF_END      = 0x0D,  // E 对局结束（下行，可广播）：winner(0xFF=平局)
  TF_RESPAWN  = 0x0E,  // R 重生（下行）
  TF_PAUSE    = 0x0F,  // P 暂停（下行，可广播）
  TF_RESUME   = 0x10,  // Q 恢复（下行，可广播）
  TF_XHIT     = 0x11,  // X 外部命中（下行）：shooter,weapon,shotSeq,channel
  TF_VITAL    = 0x12,  // V 生命同步（下行）：playerId(2B BE),hp,alive
  TF_ASSIGN   = 0x13,  // 信道+时隙分配（下行，目标 devIdx）：
                       //   payload = {channelIdx, slot, N, mapVer, spare}
                       //   slot=0xFF → 信道满，试下一信道
};

// ===== flags（低 3 位）=====
enum {
  TF_FLAG_FRAG_MORE  = 0x04,  // 后续还有分片
  TF_FLAG_FRAG_FIRST = 0x02,  // 分片首片
  TF_FLAG_HELMET     = 0x01,  // J 帧：头盔（附属设备）
};

#define TF_BROADCAST_IDX 0xFF   // 广播目标（下行全体收；信标）

// ===== 标准频点栅格（网关自动分配与设备扫描共用）=====
// 频点 = TDMA_STD_BASE_MHZ + k × TDMA_STD_STEP_MHZ（k=0..TDMA_STD_CHANNELS-1）
// 网关开机自检出 M 个射频后，从栅格中均匀取 M 个最分散频点（间隔 ≥2MHz 抗邻道干扰）；
// 设备扫描全栅格 → 只听到网关实际使用的频点 → 兼容"射频没装满"。
#define TDMA_STD_BASE_MHZ 470.0f
#define TDMA_STD_STEP_MHZ 2.0f
#define TDMA_STD_CHANNELS 20

// CRC-8/ATM：poly 0x07，init 0，无反射
uint8_t tdmaCrc8(const uint8_t *data, size_t len);

// 编码：TdmaFrame → buf[10]（计算并写入 crc）
void tdmaEncode(const TdmaFrame &f, uint8_t *buf);

// 解码：buf[10] → TdmaFrame；crc 校验通过返回 true
bool tdmaDecode(const uint8_t *buf, TdmaFrame &f);

// 便捷：从 5B payload 组装（payload 可传 NULL → 全 0）
void tdmaMake(TdmaFrame &f, uint8_t devIdx, uint8_t type, uint8_t flags,
              uint16_t seq, const uint8_t *payload5);
