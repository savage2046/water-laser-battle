#pragma once
#include <Arduino.h>
#include "config.h"
#include "TdmaProto.h"

// ============================================================================
// lora-gwtest 统计模块
//
// 写入方：射频任务（core1 优先级 5，LoRaTest）
// 读取方：主任务（loopTask，打印报告）
// 说明：不加锁。目的是人读的诊断报告，允许 ±1 帧级别的并发偏差（不会误导结论）；
//       表项只增不删，遍历安全。串口命令 r 清零。
// ============================================================================

#define LOSS_BUCKETS 6   // 连续丢失/seq 缺口分布桶：1 / 2 / 3 / 4 / 5-9 / ≥10

// ---------------------------------------------------------------- 每设备
struct DevStat {
  bool     used;
  uint8_t  idx;
  char     devId[8];          // J 帧 payload 的 5 字节 ASCII（不足补 0）

  uint32_t frames;            // 收到的合法 TDMA 帧总数
  uint32_t byType[32];        // 各帧类型计数

  // RSSI / SNR
  bool     hasRf;
  int16_t  rssiMin, rssiMax;  // dBm
  int32_t  rssiSum;
  int8_t   snrMinX4, snrMaxX4;  // SNR ×4 定点（0.25dB 步进）
  int32_t  snrSumX4;

  // 载频偏差（Hz；两块板晶振/TCXO 对齐程度）
  bool     hasFe;
  int16_t  feMin, feMax;
  int32_t  feSum;
  uint16_t feN;

  // 上行 seq 连续性 → 丢包率 PER（仅统计非 JOIN 且 seq!=0 的帧）
  bool     seqSeen;
  uint16_t seqFirst, seqLast, seqPrev;
  uint32_t seqRecv;           // 计入 PER 的收到帧数
  uint32_t seqMissing;        // 由 seq 缺口推出的丢失帧数
  uint32_t seqDup;            // 重复 seq
  uint16_t seqResets;         // seq 回退（设备重启）次数
  uint16_t gapMax;            // 最大连续缺口（= 连续丢失帧数）
  uint32_t gapHist[LOSS_BUCKETS];

  // 到达间隔（ms）
  bool     intSeen;
  uint32_t lastArrMs;
  uint32_t intMin, intMax, intSum;
  uint16_t intN;

  // TDMA：本设备帧到达时刻相对「期望时隙起点」的偏移（µs，正=晚于期望）
  bool     offSeen;
  int32_t  offMin, offMax, offSum;
  uint16_t offN;

  // TDMA：超帧信标起点 → 本设备帧到达（µs；含设备处理 + 单程空口）
  bool     respSeen;
  uint32_t respMin, respMax, respSum;
  uint16_t respN;

  // GW 侧分配与注册时延
  bool     assigned;
  uint8_t  slot;
  uint32_t assignDoneUs;      // 本设备 ASSIGN 帧发射完成时刻
  bool     firstSlotSeen;
  bool     regLatSeen;
  uint32_t regLatUs;          // ASSIGN 发射完成 → 首个时隙帧到达
};

// ---------------------------------------------------------------- 频点质量
struct FreqQual {
  float    avg;      // 瞬时 RSSI 均值（噪声底）
  int16_t  mx;       // 瞬时 RSSI 峰值
  uint16_t frames;   // 该频点解出的合法 TDMA 帧数
};

// ---------------------------------------------------------------- Ping-Pong
struct PingStat {
  bool     seen;
  uint32_t tx;        // 已发 ping
  uint32_t rx;        // 已收 pong（RTT 有效样本）
  uint32_t lost;      // 超时未回
  uint32_t unknown;   // 收到但 seq 未匹配（迟到/重复）
  uint16_t rttMin, rttMax;
  uint32_t rttSum;
  uint16_t n;         // 有效样本数（= rx，封顶 MAX_RTT_SAMPLES）
  uint16_t samples[MAX_RTT_SAMPLES];
  uint16_t lossRun;     // 当前连续丢失
  uint16_t lossRunMax;
  uint32_t lossHist[LOSS_BUCKETS];
  uint32_t lastPongMs;
};

// ---------------------------------------------------------------- 全局
struct GlobalStat {
  // 接收
  uint32_t rxOk;         // 合法（LoRa CRC 通过 + 长度合理）
  uint32_t rxCrcErr;     // LoRa 硬件 CRC / 报头错
  uint32_t rxLenErr;     // 长度异常（非 10B）
  uint32_t rxCrc8Err;    // 10B 但 CRC-8 不过
  // 发射
  uint32_t txFrames;
  uint32_t txErr;
  uint32_t airUsMin, airUsMax, airUsSum, airUsN;
  uint32_t beaconTx, dlTx, assignTx;
  uint32_t startTx, endTx;
  // 实测信标周期（µs；标称 = 30+10N ms —— 用来暴露超帧是否跑偏）
  uint32_t sfMin, sfMax, sfSum, sfN;
  // 事件缓冲溢出
  uint32_t evtDropped;
  // 频点质量
  FreqQual freq[TDMA_STD_CHANNELS];
  bool     freqValid;
  // Ping
  PingStat ping;
};

// ---------------------------------------------------------------- API
GlobalStat &statsG();

DevStat *statsDev(uint8_t idx);          // 取或新建（表满返回 NULL）
DevStat *statsDevAt(uint16_t i);         // 遍历用（含未用项，需查 used）
uint16_t statsDevUsed();
void     statsReset();

// 一帧合法 TDMA 帧：RSSI/SNR/载频偏差/seq/间隔/时隙偏移
//   slotFrame = true 表示该帧落在 TDMA 设备时隙内（GW 模式），offUs/respUs 有效
void statsOnFrame(const TdmaFrame &f, int16_t rssi, float snr, int16_t freqErrHz,
                  uint32_t nowMs, bool slotFrame, int32_t offUs, uint32_t respUs);

void statsCountRx(uint8_t kind);   // 0=合法 1=LoRaCRC错 2=长度异常 3=CRC8错
void statsOnTx(uint32_t airUs, bool ok);

void statsOnPingSent(uint16_t seq, uint32_t nowUs);
void statsOnPong(uint16_t seq, uint32_t nowUs);
void statsPingSweep(uint32_t nowUs);     // 未回超时 → 计入丢失
float statsP95Us();                      // RTT P95（无样本返回 0）

const char *tdmaTypeName(uint8_t type);  // 帧类型名（打印用）
