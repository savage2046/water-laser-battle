#include <Arduino.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "LoRaTest.h"
#include "Stats.h"
#include "TdmaProto.h"

// ============================================================================
// lora-gwtest —— 枪端 PCB 充当 LoRa 网关的 470MHz 链路测试固件
//
// 本文件负责「人机界面」：开机自检报告 / 串口命令 / 板载 LED / 心跳 / 逐帧日志。
// 射频与时隙逻辑全部在 LoRaTest（独立任务），本文件只读它的统计并在主循环打印。
//
// 串口策略（沿用 gun-selftest 已验证的做法）：
//   所有打印走 gwLog() —— 无 USB 主机时自动静默、不阻塞（可电池单供）；
//   主机接入瞬间补打完整报告，首个心跳周期再复述一次（防 USB CDC 枚举丢帧）；
//   之后每 5s 一行 #hb 心跳。
//   逐帧日志用 l 命令开关（默认开）：追求时隙定时精度时可关掉。
// ============================================================================

static bool     g_frameLog = FRAME_LOG_DEFAULT;
static bool     g_hostSeen = false;
static bool     g_reportPending = true;
static uint32_t g_lastRxMs = 0;
static uint32_t g_lastScanPass = 0;
static uint32_t g_bootMs = 0;

// ---------------------------------------------------------------- 串口
void gwLog(const char *fmt, ...) {
  if (!(bool)Serial) return;   // 无主机：静默（不阻塞、可电池单供）
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

static void hexStr(const uint8_t *b, uint8_t n, char *out, size_t cap) {
  size_t p = 0;
  out[0] = 0;
  for (uint8_t i = 0; i < n && (p + 4) < cap; i++) {
    p += (size_t)snprintf(out + p, cap - p, "%02X ", b[i]);
  }
  if (p > 0) out[p - 1] = 0;
}

static const char *errName(int code) {
  switch (code) {
    case RADIOLIB_ERR_NONE:             return "NONE";
    case RADIOLIB_ERR_CHIP_NOT_FOUND:   return "CHIP_NOT_FOUND：芯片无应答 —— CS/MISO/BUSY/供电/焊接";
    case RADIOLIB_ERR_SPI_WRITE_FAILED: return "SPI_WRITE_FAILED：读写回读不一致 —— MOSI/MISO/SCLK/焊接";
    case RADIOLIB_ERR_SPI_CMD_TIMEOUT:  return "SPI_CMD_TIMEOUT：命令超时 —— BUSY(G45)";
    case RADIOLIB_ERR_SPI_CMD_INVALID:  return "SPI_CMD_INVALID";
    case RADIOLIB_ERR_SPI_CMD_FAILED:   return "SPI_CMD_FAILED";
    case RADIOLIB_ERR_TX_TIMEOUT:       return "TX_TIMEOUT";
    case RADIOLIB_ERR_RX_TIMEOUT:       return "RX_TIMEOUT";
    default:                            return "见 RadioLib TypeDef.h";
  }
}

// ---------------------------------------------------------------- 报告
static void printScanTable() {
  const GlobalStat &g = statsG();
  if (!g.freqValid) {
    gwLog("  [扫频] 尚未完成（进行中）\n");
    return;
  }
  gwLog("  [扫频] 20 点标准栅格（470.0 + k×2.0 MHz；每点 %d×%dms 采样 + %dms 驻留）\n",
        SCAN_SAMPLES, SCAN_INTERVAL_MS, SCAN_DWELL_MS);
  int8_t bestK = -1;
  uint16_t bestF = 0;
  for (uint8_t k = 0; k < TDMA_STD_CHANNELS; k++) {
    const FreqQual &q = g.freq[k];
    if (q.frames > bestF) { bestF = q.frames; bestK = (int8_t)k; }
    const bool noisy = (q.mx > SCAN_NOISY_MAX_DBM || q.avg > SCAN_NOISY_AVG_DBM);
    gwLog("    k=%2u %6.1fMHz  底噪 avg=%6.1f max=%4d dBm  帧=%3u%s\n", (unsigned)k,
          (double)(TDMA_STD_BASE_MHZ + (float)k * TDMA_STD_STEP_MHZ),
          (double)q.avg, (int)q.mx, (unsigned)q.frames,
          q.frames > 0 ? "  <== 有合法 TDMA 帧" : (noisy ? "  <-- 有能量但解不出帧" : ""));
  }
  if (bestK >= 0) {
    gwLog("    结论：占用频点 k=%d (%.1fMHz)，%u 帧 → 建议 GW/MON 用该频点\n",
          (int)bestK,
          (double)(TDMA_STD_BASE_MHZ + (float)bestK * TDMA_STD_STEP_MHZ),
          (unsigned)bestF);
  } else {
    gwLog("    结论：20 个频点均未解出合法 TDMA 帧（对端未上电/太远/天线未接/频点不符）\n");
  }
}

static void printRunState() {
  gwLog("  [运行] 模式 = %s；频点 k=%u (%.1fMHz)；当前底噪 = %.1f dBm\n",
        LoRaTest::modeName(g_test.mode()), (unsigned)g_test.grid(),
        (double)(TDMA_STD_BASE_MHZ + (float)g_test.grid() * TDMA_STD_STEP_MHZ),
        (double)g_test.idleRssi());
  if (g_test.mode() == MODE_GW) {
    gwLog("         TDMA 超帧 = 30+10N = %lums；mapVer=%u N=%u；信标已发 %lu；"
          "下行 %lu（ASSIGN %lu）\n",
          (unsigned long)g_test.sfMs(), (unsigned)g_test.mapVer(),
          (unsigned)g_test.n(), (unsigned long)statsG().beaconTx,
          (unsigned long)statsG().dlTx, (unsigned long)statsG().assignTx);
    gwLog("         设备表：%u 台", (unsigned)g_test.devCount());
    for (uint16_t i = 0; i < statsDevUsed(); i++) {
      DevStat *d = statsDevAt(i);
      if (!d || !d->used || !d->assigned) continue;
      gwLog("  dev%u→slot%u", (unsigned)d->idx, (unsigned)d->slot);
    }
    gwLog("\n");
    if (statsG().sfN) {
      const uint32_t avg = statsG().sfSum / statsG().sfN;
      gwLog("         实测信标周期 min/avg/max = %lu/%lu/%lu us (n=%lu)"
            "（标称 %luus + 注册窗裕量 %dus）%s\n",
            (unsigned long)statsG().sfMin, (unsigned long)avg,
            (unsigned long)statsG().sfMax, (unsigned long)statsG().sfN,
            (unsigned long)g_test.sfMs() * 1000UL, GW_REG_RX_EXTRA_US,
            (statsG().sfMax - statsG().sfMin) > 10000
                ? "  <-- 周期抖动过大，检查 DL 队列/打印负载"
                : "");
    }
  }
}

static void printDevTable() {
  const uint16_t n = statsDevUsed();
  gwLog("  [设备统计] 共 %u 台\n", (unsigned)n);
  if (n == 0) {
    gwLog("       （尚未收到任何帧）\n");
    return;
  }
  for (uint16_t i = 0; i < n; i++) {
    DevStat *d = statsDevAt(i);
    if (!d || !d->used) continue;

    // 类型分布
    char tbuf[128];
    tbuf[0] = 0;
    size_t p = 0;
    for (uint8_t t = 0; t < 32; t++) {
      if (!d->byType[t] || (p + 20) >= sizeof(tbuf)) continue;
      p += (size_t)snprintf(tbuf + p, sizeof(tbuf) - p, " %s=%lu",
                            tdmaTypeName(t), (unsigned long)d->byType[t]);
    }
    gwLog("    dev=%-3u %-7s 帧=%lu%s\n", (unsigned)d->idx,
          d->devId[0] ? d->devId : "-", (unsigned long)d->frames, tbuf);

    if (d->hasRf) {
      gwLog("         RSSI %d/%d dBm (avg %.1f)  SNR %.2f/%.2f dB (avg %.2f)",
            (int)d->rssiMin, (int)d->rssiMax,
            (double)d->rssiSum / (double)d->frames,
            (double)d->snrMinX4 / 4.0, (double)d->snrMaxX4 / 4.0,
            (double)d->snrSumX4 / 4.0 / (double)d->frames);
      if (d->feN) {
        gwLog("  载偏 avg %ld Hz", (long)(d->feSum / (int32_t)d->feN));
      }
      gwLog("\n");
    }

    if (d->seqRecv) {
      const uint32_t denom = d->seqRecv + d->seqMissing;
      gwLog("         上行 seq 首/末=%u/%u 收=%lu 缺=%lu → PER %.2f%%  最大连续缺口=%u"
            "  重复=%lu 复位=%u\n",
            (unsigned)d->seqFirst, (unsigned)d->seqLast,
            (unsigned long)d->seqRecv, (unsigned long)d->seqMissing,
            denom ? (100.0 * (double)d->seqMissing / (double)denom) : 0.0,
            (unsigned)d->gapMax, (unsigned long)d->seqDup,
            (unsigned)d->seqResets);
      gwLog("         连续缺口分布(gap=丢失帧数) 1:%lu 2:%lu 3:%lu 4:%lu 5-9:%lu >=10:%lu\n",
            (unsigned long)d->gapHist[0], (unsigned long)d->gapHist[1],
            (unsigned long)d->gapHist[2], (unsigned long)d->gapHist[3],
            (unsigned long)d->gapHist[4], (unsigned long)d->gapHist[5]);
    }

    if (d->intSeen && d->intN) {
      gwLog("         到达间隔 min/max/avg = %lu/%lu/%lu ms (n=%u)\n",
            (unsigned long)d->intMin, (unsigned long)d->intMax,
            (unsigned long)(d->intSum / d->intN), (unsigned)d->intN);
    }

    if (d->offSeen && d->offN) {
      gwLog("         时隙 slot=%u 偏移 min/max/avg = %ld/%ld/%ld us (n=%u)%s\n",
            (unsigned)(d->assigned ? d->slot : 0xFF), (long)d->offMin,
            (long)d->offMax, (long)(d->offSum / (int32_t)d->offN),
            (unsigned)d->offN,
            (d->offMax - d->offMin) > 20000 ? "  <-- 抖动大（检查对端时钟/距离）" : "");
    }
    if (d->respN) {
      gwLog("         信标→本设备帧 min/max/avg = %lu/%lu/%lu us (n=%u)\n",
            (unsigned long)d->respMin, (unsigned long)d->respMax,
            (unsigned long)(d->respSum / d->respN), (unsigned)d->respN);
    }
    if (d->regLatSeen) {
      gwLog("         注册时延（ASSIGN 发完 → 首个时隙帧）= %lu us\n",
            (unsigned long)d->regLatUs);
    }
  }
}

static void printGlobal() {
  const GlobalStat &g = statsG();
  gwLog("  [接收] 合法帧=%lu  LoRa CRC 错=%lu  长度异常=%lu  CRC8 错=%lu  事件丢弃=%lu\n",
        (unsigned long)g.rxOk, (unsigned long)g.rxCrcErr, (unsigned long)g.rxLenErr,
        (unsigned long)g.rxCrc8Err, (unsigned long)g.evtDropped);
  if (g.airUsN) {
    gwLog("  [发射] 帧=%lu 失败=%lu  空口耗时 min/avg/max = %lu/%lu/%lu us"
          "（理论 getTimeOnAir(10) = %lu us）\n",
          (unsigned long)g.txFrames, (unsigned long)g.txErr,
          (unsigned long)g.airUsMin, (unsigned long)(g.airUsSum / g.airUsN),
          (unsigned long)g.airUsMax, (unsigned long)g_test.theoreticalAirUs());
  } else {
    gwLog("  [发射] 帧=%lu 失败=%lu（尚无成功发射样本）\n",
          (unsigned long)g.txFrames, (unsigned long)g.txErr);
  }
  gwLog("         信标=%lu  下行=%lu（ASSIGN=%lu） START/END=%lu/%lu\n",
        (unsigned long)g.beaconTx, (unsigned long)g.dlTx,
        (unsigned long)g.assignTx, (unsigned long)g.startTx,
        (unsigned long)g.endTx);

  const PingStat &p = g.ping;
  if (p.tx || p.rx) {
    const uint32_t decided = p.lost + p.rx;
    gwLog("  [Ping] 发=%lu 收=%lu 丢=%lu 未匹配=%lu → 丢包率 %.2f%%\n",
          (unsigned long)p.tx, (unsigned long)p.rx, (unsigned long)p.lost,
          (unsigned long)p.unknown,
          decided ? (100.0 * (double)p.lost / (double)decided) : 0.0);
    if (p.rx) {
      const double avg = (double)p.rttSum / (double)p.rx;
      const double air2 = 2.0 * (double)g_test.theoreticalAirUs();
      gwLog("         RTT min/avg/max/P95 = %.2f/%.2f/%.2f/%.2f ms"
            "（原始，含双程空口 ≈%.1fms）\n",
            (double)p.rttMin / 1000.0, avg / 1000.0, (double)p.rttMax / 1000.0,
            (double)statsP95Us() / 1000.0, air2 / 1000.0);
      gwLog("         RTT 净（扣除双程空口）= %.2f/%.2f/%.2f/%.2f ms\n",
            ((double)p.rttMin - air2) / 1000.0, (avg - air2) / 1000.0,
            ((double)p.rttMax - air2) / 1000.0,
            ((double)statsP95Us() - air2) / 1000.0);
    }
    gwLog("         连续丢失 max=%u  分布 1:%lu 2:%lu 3:%lu 4:%lu 5-9:%lu >=10:%lu\n",
          (unsigned)p.lossRunMax, (unsigned long)p.lossHist[0],
          (unsigned long)p.lossHist[1], (unsigned long)p.lossHist[2],
          (unsigned long)p.lossHist[3], (unsigned long)p.lossHist[4],
          (unsigned long)p.lossHist[5]);
    if (p.unknown) {
      gwLog("         （未匹配 %lu 个 PONG：可能是迟到的应答或对端 seq 复用）\n",
            (unsigned long)p.unknown);
    }
  }
}

static void printFullReport() {
  gwLog("\n[lora-gwtest] ====== LoRa 网关链路测试报告 (v%s, 编译 %s %s) ======\n",
        FW_VERSION, __DATE__, __TIME__);
  gwLog("  MCU=%s  Flash=%uMB  SDK=%s  运行=%lus\n", ESP.getChipModel(),
        (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)),
        ESP.getSdkVersion(),
        (unsigned long)((millis() - g_bootMs) / 1000));

  // T1 LED
  gwLog("  [T1] 板载 LED（G%d，低电平点亮）—— 程序运行中\n", PIN_LED);

  // T2 LoRa 模块
  if (!g_test.ready()) {
    gwLog("  [T2] LoRa 模块初始化失败：begin=%d（%s）\n", (int)g_test.beginCode(),
          errName(g_test.beginCode()));
    gwLog("       ⚠️ 已自动试过 SX1262/SX1268/LLCC68 三个类（按版本字符串校验身份），\n");
    gwLog("          所以**不是类选错**，而是芯片真的没应答 —— 按序查：\n");
    gwLog("          ① U2 供电 3.3V/GND（万用表量模组脚）② CS=G16 是否连到 U2.NSS\n");
    gwLog("          ③ BUSY=G45 是否连 U2.BUSY（悬空/被拉死会让命令超时）④ RST=G46\n");
    gwLog("          ⑤ SCK=G42/MOSI=G15/MISO=G41 是否虚焊错位 ⑥ U2 是否贴反/漏焊\n");
    gwLog("#RESULT lora=FAIL (begin=%d)\n", (int)g_test.beginCode());
  } else {
    const uint8_t *v = g_test.versionReg();
    char vs[17];
    memcpy(vs, v, 16);
    vs[16] = 0;
    for (uint8_t i = 0; i < 16; i++) {
      if (vs[i] < 0x20 || vs[i] > 0x7E) vs[i] = '.';
    }
    gwLog("  [T2] LoRa 模块 OK\n");
    gwLog("       RadioLib 类 = %s（自动探测命中）\n", g_test.chipClass());
    gwLog("       芯片=%s  版本寄存器 0x0320=\"%s\"\n", g_test.chipName(), vs);
    gwLog("       配置 %.1fMHz BW%.0fk SF%d CR4/%d sync=0x%02X %ddBm 前导%d"
          "  → begin=0；理论空口(10B)=%luus\n",
          (double)RADIO_FREQ_MHZ, (double)RADIO_BW_KHZ, (int)RADIO_SF,
          (int)RADIO_CR, RADIO_SYNC_WORD, (int)RADIO_TX_POWER_DBM,
          (int)RADIO_PREAMBLE_LEN, (unsigned long)g_test.theoreticalAirUs());
    gwLog("       引脚 CS=G%d SCK=G%d MOSI=G%d MISO=G%d RST=G%d BUSY=G%d；",
          PIN_SX_NSS, PIN_SX_SCLK, PIN_SX_MOSI, PIN_SX_MISO, PIN_SX_RST,
          PIN_SX_BUSY);
#if PIN_SX_DIO1 >= 0
    gwLog("DIO1=G%d 中断收包（到达时刻 ±µs，已触发 %lu 次）\n", PIN_SX_DIO1,
          (unsigned long)g_test.irqCount());
#else
    gwLog("DIO1 未接线（收发轮询，到达时刻取轮询时刻）\n");
#endif
    gwLog("#RESULT lora=PASS\n");
  }

  printScanTable();
  printRunState();
  printDevTable();
  printGlobal();

  gwLog("  [LED] 快闪120ms=射频初始化失败；慢闪1s=GW模式；500ms=其它模式；收帧瞬间短亮\n");
  gwLog("#ready 命令 h 看帮助；i 打印本报告；r 清零统计；s 重新扫频；x 重启\n");
}

static void printHelp() {
  gwLog("\n[lora-gwtest] 串口命令（行末回车）\n");
  gwLog("  h / ?      本帮助\n");
  gwLog("  m          循环切换模式：SCAN→MON→GW→PING→ECHO\n");
  gwLog("  m0..m4     直接指定：0 SCAN 1 MON 2 GW 3 PING 4 ECHO\n");
  gwLog("  k<0-19>    设定频点栅格（470.0 + k×2.0 MHz），如 k7\n");
  gwLog("  s          重新扫频（自动回到 SCAN）\n");
  gwLog("  i          立即打印完整统计报告\n");
  gwLog("  r          清零全部统计\n");
  gwLog("  p          发 %d 个 ping；p<n> 发 n 个；p0 连续 ping\n",
        PING_BATCH_DEFAULT);
  gwLog("  l          逐帧日志（#rx 行）开/关\n");
  gwLog("  t / e      广播 TF_START / TF_END（可选：促使枪端进入对局、产生更多帧）\n");
  gwLog("            ⚠ 会真实影响对端枪端状态；MON 为被动模式，t/e 无效\n");
  gwLog("  x          重启\n");
  gwLog("当前：模式=%s k=%u 逐帧日志=%s\n\n", LoRaTest::modeName(g_test.mode()),
        (unsigned)g_test.grid(), g_frameLog ? "开" : "关");
}

// ---------------------------------------------------------------- 逐帧日志
static void printEvt(const RxEvt &e) {
  const uint32_t s = e.ms / 1000, ms3 = e.ms % 1000;
  if (e.tdmaOk) {
    char hex[64];
    hexStr(e.raw, e.n, hex, sizeof(hex));
    DevStat *d = statsDev(e.f.devIdx);
    gwLog("#rx t=%lu.%03lus k=%u dev=%-3u %-7s type=%-9s seq=%-5u fl=%u "
          "rssi=%d snr=%.2f fe=%dHz len=%u",
          (unsigned long)s, (unsigned long)ms3, (unsigned)e.grid,
          (unsigned)e.f.devIdx, (d && d->devId[0]) ? d->devId : "-",
          tdmaTypeName(e.f.type), (unsigned)e.f.seq, (unsigned)e.f.flags,
          (int)e.rssi, (double)e.snrX4 / 4.0, (int)e.freqErrHz, (unsigned)e.len);
    if (e.slotFrame) {
      gwLog(" off=%+ldus resp=%luus", (long)e.offUs, (unsigned long)e.respUs);
    }
    gwLog(" pl=%02X%02X%02X%02X%02X [%s]\n", e.f.payload[0], e.f.payload[1],
          e.f.payload[2], e.f.payload[3], e.f.payload[4], hex);
  } else if (e.crcOk) {
    char hex[64];
    hexStr(e.raw, e.n, hex, sizeof(hex));
    gwLog("#rxLEN t=%lu.%03lus k=%u len=%u（非 10B，同频其它 LoRa 网络？）"
          " rssi=%d snr=%.2f hex=[%s]\n",
          (unsigned long)s, (unsigned long)ms3, (unsigned)e.grid,
          (unsigned)e.len, (int)e.rssi, (double)e.snrX4 / 4.0, hex);
  } else {
    char hex[64];
    hexStr(e.raw, e.n, hex, sizeof(hex));
    gwLog("#rxBAD t=%lu.%03lus k=%u len=%u crc=BAD rssi=%d snr=%.2f hex=[%s]\n",
          (unsigned long)s, (unsigned long)ms3, (unsigned)e.grid,
          (unsigned)e.len, (int)e.rssi, (double)e.snrX4 / 4.0, hex);
  }
}

// ---------------------------------------------------------------- 命令
static void execCmd(const char *cmd) {
  const char c = cmd[0];
  if (!c) return;
  const long arg = atol(cmd + 1);
  switch (c) {
    case 'h':
    case '?':
      printHelp();
      break;
    case 'm': {
      if (cmd[1] >= '0' && cmd[1] <= '4') {
        g_test.setMode((TestMode)arg);
      } else {
        g_test.setMode((TestMode)(((int)g_test.mode() + 1) % MODE_COUNT));
      }
      gwLog("#cmd mode=%s\n", LoRaTest::modeName(g_test.mode()));
      break;
    }
    case 'k':
      g_test.setGrid((uint8_t)((arg < 0) ? 0 : arg));
      gwLog("#cmd grid k=%u (%.1fMHz)\n", (unsigned)g_test.grid(),
            (double)(TDMA_STD_BASE_MHZ + (float)g_test.grid() * TDMA_STD_STEP_MHZ));
      break;
    case 's':
      g_test.rescan();
      gwLog("#cmd rescan\n");
      break;
    case 'i':
      printFullReport();
      break;
    case 'r':
      statsReset();
      g_lastScanPass = 0;
      gwLog("#cmd stats reset\n");
      printFullReport();
      break;
    case 'p':
      if (cmd[1] == 0) {
        g_test.sendPing(PING_BATCH_DEFAULT);
        gwLog("#cmd ping x%d（模式=%s）\n", PING_BATCH_DEFAULT,
              LoRaTest::modeName(g_test.mode()));
      } else {
        g_test.sendPing((uint16_t)arg);
        gwLog("#cmd ping %s\n", arg == 0 ? "连续" : cmd + 1);
      }
      break;
    case 'l':
      g_frameLog = !g_frameLog;
      gwLog("#cmd 逐帧日志=%s\n", g_frameLog ? "开" : "关");
      break;
    case 't':
      g_test.broadcastStart();
      gwLog("#cmd 广播 START\n");
      break;
    case 'e':
      g_test.broadcastEnd();
      gwLog("#cmd 广播 END\n");
      break;
    case 'x':
      gwLog("#cmd 重启…\n");
      delay(200);
      ESP.restart();
      break;
    default:
      gwLog("#cmd 未知命令 \"%s\"（h 看帮助）\n", cmd);
      break;
  }
}

static char s_line[32];
static uint8_t s_lineLen = 0;

static void handleSerial() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (s_lineLen) {
        s_line[s_lineLen] = 0;
        s_lineLen = 0;
        execCmd(s_line);
      }
      continue;
    }
    if (c == 8 || c == 127) {   // 退格
      if (s_lineLen) s_lineLen--;
      continue;
    }
    if (s_lineLen < sizeof(s_line) - 1) s_line[s_lineLen++] = c;
  }
}

// ---------------------------------------------------------------- LED
static void ledUpdate(uint32_t now) {
  uint32_t half;
  if (!g_test.ready()) {
    half = 120;                                   // 射频初始化失败：快闪
  } else if (g_test.mode() == MODE_GW) {
    half = 1000;                                  // GW：慢闪（正在发信标）
  } else {
    half = 500;
  }
  bool on = ((now / half) & 1u) == 0;
  if (g_lastRxMs && (now - g_lastRxMs) < 30) on = true;   // 收帧瞬间短亮
  digitalWrite(PIN_LED, on ? LOW : HIGH);         // 低电平点亮
}

// ---------------------------------------------------------------- setup/loop
void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);   // 灭

  Serial.begin(SERIAL_BAUD);
  delay(200);
  g_bootMs = millis();

  // 上电三闪：不接串口也能确认代码已启动
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_LED, LOW);  delay(120);
    digitalWrite(PIN_LED, HIGH); delay(120);
  }

  statsReset();
  if (g_test.begin()) {
    g_test.start();              // 射频任务启动 → 默认 MODE_SCAN 全频点扫描
  }
  printFullReport();
}

void loop() {
  const uint32_t now = millis();

  // 主机后接入 → 补打一次完整报告（等 300ms 让 CDC 端点就绪，避免丢帧）
  if (!g_hostSeen && (bool)Serial) {
    g_hostSeen = true;
    delay(300);
    printFullReport();
    printHelp();
  }

  // 提示（射频任务产生：模式切换/JOIN/重排/扫频结论）
  char noteTxt[NOTE_LEN];
  while (g_test.popNote(noteTxt, sizeof(noteTxt))) {
    gwLog("#note %s\n", noteTxt);
  }

  // 逐帧事件
  RxEvt e;
  while (g_test.popEvt(e)) {
    g_lastRxMs = e.ms;
    if (g_frameLog) printEvt(e);
  }

  // 扫频完成 → 自动打一次频点表
  if (g_test.scanPassDone() != g_lastScanPass) {
    g_lastScanPass = g_test.scanPassDone();
    gwLog("#scan pass complete\n");
    printScanTable();
  }

  // 心跳（首个周期复述完整报告：开机瞬间/枚举窗口的打印可能已丢）
  static uint32_t lastHb = 0;
  if (now - lastHb >= HB_MS) {
    lastHb = now;
    if (g_reportPending) {
      g_reportPending = false;
      printFullReport();
    }
    gwLog("#hb t=%lus mode=%s k=%u devs=%u rx=%lu tx=%lu beacon=%lu "
          "ping=%lu/%lu rssi=%d\n",
          (unsigned long)(now / 1000), LoRaTest::modeName(g_test.mode()),
          (unsigned)g_test.grid(), (unsigned)g_test.devCount(),
          (unsigned long)statsG().rxOk, (unsigned long)statsG().txFrames,
          (unsigned long)statsG().beaconTx, (unsigned long)statsG().ping.rx,
          (unsigned long)statsG().ping.tx, (int)g_test.idleRssi());
  }

  ledUpdate(now);
  handleSerial();
  delay(2);
}
