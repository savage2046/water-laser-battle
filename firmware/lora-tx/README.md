# lora-tx —— 只发不收：SX1268 LoRa 470MHz 发送端

> 上电自动**每秒发一包** `LRTESTnn`（8 字节 ASCII），对端烧 **`lora-rx`**（只收）。
> 射频参数逐项一致：**470.000MHz / SF7 / BW500k / CR4-5 / 前导4 / 私有同步字 0x12 / CRC 开 / 标准 IQ**。
>
> 2026-09-12 起**枪端与网关接线相同**（都是 ESP32-S3），两块板烧同一个 env。

## 接线（枪端 / 网关通用）

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| NSS | G16 | SPI 片选（低有效，空闲高） |
| SCK | G42 | SPI 时钟（S3 上也是 JTAG 脚，接外部调试器才冲突） |
| MOSI | G15 | SPI 主机输出 |
| MISO | G41 | **模组 → ESP32**，只做输入 |
| RST | G46 | 复位（低有效，S3 strapping 脚） |
| BUSY | G45 | 忙指示（只做输入，发命令前必须等它变低） |

## 上电输出（纯 ASCII）

```
===== lora-tx : SX1268 LoRa TX only (470MHz SF7 BW500k) =====
board : gun-board / gateway（ESP32-S3，接线相同）
spi   : NSS=G16 SCK=G42 MOSI=G15 MISO=G41  mode0  1000kHz
#cfg standby / packetType=LoRa / DIO2-as-RF-switch=1 (0x9D 01) / IrqMask=0x03FF (0x08) / PaConfig=04 07 00 01 (0x95) / TxParams=pwr 15 ramp 200us (0x8E)
#cfg rfFreq=0x1D600000 (470.000MHz) / SF7 BW500k CR4-5 / preamble 4 explicit CRC on / len 8
#toa expected = 8.00ms for 8 bytes (SF7/BW500k/CR4-5/preamble 4/CRC on)
#st  st=0x22(STBY_RC)  pkt=0x01(LoRa)  err=0x0000  tx: ok=0 err=0  air min/avg/max=0.00/0.00/0.00 ms  period=1000ms  busyTo=0
    ver=SX1268 V2F 2F02  sync=0x1424  autoTx=ON
#tx  seq=1 data="LRTEST01"  irq=0x0001 TX_DONE  air=8.04ms (expected 8.00ms)
```

## 命令字节（全部按手册 §13，括号为表号）

| 命令 | 字节 | 表 |
| --- | --- | --- |
| `SetStandby` | `80 00` → STDBY_RC | 13-5 |
| `SetPacketType` | `8A 01` → LoRa | 13-37/38 |
| **`SetDio2AsRfSwitchCtrl`** | **`9D 01`** → DIO2 驱动 RF 开关（RX 时 DIO2=0、TX 时 DIO2=1） | **13-32/13-33** |
| **`SetDioIrqParams`** | **`08 03 FF 00 00 00 00 00 00`** → IrqMask=0x03FF，DIO1/2/3 不映射 | **13-28** |
| **`SetPaConfig`** | **`95 04 07 00 01`** → paDutyCycle/hpMax/deviceSel/paLut（高功率档） | **13-20/13-21** |
| **`SetTxParams`** | **`8E 0F 04`** → +15dBm / 上升时间 200µs | **13-40/13-41** |
| `SetRfFrequency` | `86 1D 60 00 00` → 470.000MHz（`RfFreq=f×2²⁵/32e6`） | 13-36 |
| `SetModulationParams` | `8B 07 06 01 00` → SF7 / BW500k / CR4-5 / LDRO关 | 13-47/48/49/50 |
| `SetPacketParams` | `8C 0004 00 08 01 00` → 前导4 / 显式头 / 长度8 / CRC开 / 标准IQ | 13-66…70 |
| `SetBufferBaseAddress` | `8F 00 00` | 13-75 |
| `WriteBuffer` | `0E 00 <8字节>` | 13-26 |
| `SetTx` | `83 00 00 00`（timeout 单位 15.625µs，`0`=不启用，发完自动回 STDBY_RC） | 13-6/7 |
| `GetIrqStatus` | `12 NOP NOP` → `IrqStatus` 在 **rx[2..3]**，`TxDone`=bit0 | 13-30 / 13-29 |

> 加粗的四条是**必发命令**：漏发后的具体现象（TX_DONE 正常但空口无辐射 / `irq=0x0000` / 默认低功率档）
> 以及 RadioLib 的对应 call site，见 `docs/lora-gateway-test.md` §4.7。

## 判读

| 现象 | 含义 |
| --- | --- |
| `irq=0x0001 TX_DONE`，`air≈8.00ms` | ✅ 正常发出（空口时间对得上 = 配置全对） |
| `irq=0x0000 no-irq but status=0x2C (cmd=TX done)` | ⚠ 发出去了但 IRQ 没记录 → 查 `0x08`（IRQ 屏蔽） |
| `irq=0x0200 TIMEOUT` / `status=... FAILED` | ❌ 没发出去：查供电（PA 瞬时电流）、`0x9D`、`0x95`/`0x8E` |
| `air` 远大于 8ms（如 40ms+） | ⚠ 在等 DIO1 电平超时（阻塞 `transmit()` 在没接 DIO1 时白等 5×空口） |
| `st` 一直是 `TX` | ⚠ 卡在发射态（正常发完自动回 `STBY_RC`） |
| `err` 非 0 | ⚠ PLL/校准出错，查频率参数与晶振 |
| `busyTo` 一直涨 | ⚠ BUSY 线不通 |
| 发送端全正常、接收端只出 `CRC error` | ⚠ 收到了但解不出：同步字/参数不一致、太远、或干扰 |
| 发送端全正常、接收端**什么都没有** | ⚠ 先查 `0x9D 01` 与 `0x08`，再查天线 |

## 命令（行末回车）

| 命令 | 作用 |
| --- | --- |
| `t` | 立刻发一包 |
| `T` | 自动发送 开/关（默认 **ON**，每 1000ms 一包） |
| `p` | 周期切换 200 / 500 / 1000 / 2000 ms |
| `a` | 打印一行状态（st/pkt/err/计数/空口时间 min-avg-max） |
| `r` | 重新配置 |
| `s` | SPI 时钟换档 1M → 2M → 4M → 8M → 16M |
| `0740` / `0320.16` | 读寄存器（带原始字节回显） |
| `h` | 帮助 |

## 双板测试

```bash
cd firmware/lora-tx && pio run -e gun-s3 -t upload     # 板 A（发）
cd firmware/lora-rx && pio run -e gun-s3 -t upload     # 板 B（收）
```

1. A 板上电 → 每秒一行 `#tx ... TX_DONE air=8.0xms`；
2. B 板应开始刷 `#rx #n len=8 rssi=-4x.xdBm snr=9.xxdB data="LRTESTnn"` → **链路通**；
3. 逐渐拉远，看 `rssi`/`snr` 衰减、`PER` 上涨（B 板每秒汇总一行）；
4. 按 `p` 把周期调到 200ms 做压力测试（5 包/秒）。

## 烧录

```bash
cd firmware/lora-tx
pio run -e gun-s3 -t upload      # 枪端 / 网关板都用这个 env
pio device monitor -b 115200
```

## 文件

| 文件 | 内容 |
| --- | --- |
| `platformio.ini` | 单 env `gun-s3`（枪端与网关接线相同）；只用 Arduino 核心 + SPI，无外部库 |
| `src/config.h` | 引脚、射频参数（频率/SF/BW/CR/前导/CRC/IQ/功率）、命令码与寄存器地址 |
| `src/main.cpp` | 配置序列、发包与 TxDone 轮询、空口时间统计、串口命令 |
