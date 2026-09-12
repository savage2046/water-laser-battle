# lora-rx —— 只收不发：SX1268 LoRa 470MHz 接收端

> 上电进入**连续接收**，每秒打印底噪 RSSI 与计数；收到包立刻打印
> `len / rssi / snr / sigRssi / 数据(hex+ascii)`，并按载荷序号算 **PER**。
> 对端烧 **`lora-tx`**（只发）。射频参数逐项一致：
> **470.000MHz / SF7 / BW500k / CR4-5 / 前导4 / 私有同步字 0x12 / CRC 开 / 标准 IQ**。
>
> 2026-09-12 起**枪端与网关接线相同**（都是 ESP32-S3），两块板烧同一个 env。

## 接线（枪端 / 网关通用）

| 信号 | GPIO | 说明 |
| --- | --- | --- |
| NSS | G16 | SPI 片选（低有效，空闲高） |
| SCK | G42 | SPI 时钟 |
| MOSI | G15 | SPI 主机输出 |
| MISO | G41 | **模组 → ESP32**，只做输入 |
| RST | G46 | 复位（低有效） |
| BUSY | G45 | 忙指示（只做输入） |

## 上电输出（纯 ASCII）

```
===== lora-rx : SX1268 LoRa RX only (470MHz SF7 BW500k) =====
board : gun-board / gateway（ESP32-S3，接线相同）
spi   : NSS=G16 SCK=G42 MOSI=G15 MISO=G41  mode0  1000kHz
#cfg standby / packetType=LoRa / DIO2-as-RF-switch=1 (0x9D 01) / IrqMask=0x03FF (0x08) / rfFreq=0x1D600000 (470.000MHz) / SF7 BW500k CR4-5 / preamble 4 explicit CRC on / maxLen 8 / SetRx=continuous
#1 470.000MHz rssi=-112.5dBm  st=0x52(RX)  pkt=0x01(LoRa)  err=0x0000  stats: pkts=0 crc=0 hdr=0 | mine: rx=0 crc=0 hdr=0 to=0 | busyTo=0
   ver=SX1268 V2F 2F02  sync=0x1424
#rx  #1 len=8  rssi=-31.0dBm  snr=12.50dB  sigRssi=-32.0dBm  data="LRTEST01"  (4C 52 54 45 53 54 30 31)
   PER: rx=19 miss=0 dup=0 -> 0.00%   lastPkt rssi=-31.0dBm snr=12.50dB  ago=137ms
```

## 命令字节（全部按手册 §13，括号为表号）

| 命令 | 字节 | 表 |
| --- | --- | --- |
| `SetStandby` | `80 00` → STDBY_RC | 13-5 |
| `SetPacketType` | `8A 01` → LoRa | 13-37/38 |
| **`SetDio2AsRfSwitchCtrl`** | **`9D 01`** → DIO2 驱动 RF 开关 | **13-32/13-33** |
| **`SetDioIrqParams`** | **`08 03 FF 00 00 00 00 00 00`** → IrqMask=0x03FF | **13-28** |
| `SetRfFrequency` | `86 1D 60 00 00` → 470.000MHz | 13-36 |
| `SetModulationParams` | `8B 07 06 01 00` → SF7 / BW500k / CR4-5 / LDRO关 | 13-47…50 |
| `SetPacketParams` | `8C 0004 00 08 01 00` → 前导4 / 显式头 / **最大长度8** / CRC开 / 标准IQ | 13-66…70 |
| `SetRx` | `82 FF FF FF` → 连续接收 | 13-8/9 |
| `GetIrqStatus` | `12 NOP NOP` → `IrqStatus` 在 **rx[2..3]**（`RxDone`=bit1, `HeaderErr`=bit5, `CrcErr`=bit6） | 13-30 / 13-29 |
| `ClearIrqStatus` | `02 <mask16>` | 13-31 |
| `GetRxBufferStatus` | `13` → rx[2]=`PayloadLengthRx`, rx[3]=`RxStartBufferPointer` | 13-79 |
| `ReadBuffer` | `1E <off> NOP(Status) N×NOP` → 数据从 **rx[3]** 起 | 13-27 |
| `GetPacketStatus` | `14` → rx[2]=`RssiPkt`, rx[3]=`SnrPkt`, rx[4]=`SignalRssiPkt` | 13-80 |
| `GetRssiInst` | `15` → rx[2]，`dBm = −raw/2`（raw 无符号） | 13-82 |
| `GetStats` | `10` → rx[2..7]：收包 / CRC错 / 包头错 三个 16 位计数 | 13-83 |

换算：`RSSI = −raw/2`、`SNR = SnrPkt/4`（二进制补码）。
**共同规律：返回数据的命令，命令/地址之后先有一个 `Status` 字节，数据在它后面。**

> 加粗的两条是**必发命令**；漏发 `0x9D 01` 的症状是**底噪一个字节都不变地卡死**
> （看到的是被隔离的前端），漏发 `0x08` 的症状是**对端在发、本端什么都收不到**。
> 细节见 `docs/lora-gateway-test.md` §4.7。

## 判读

| 现象 | 含义 |
| --- | --- |
| 底噪 **−100 ~ −125dBm 且在抖动** | ✅ RF 开关正常，看到的是真天线 |
| 底噪**严格不变**（如一直 −88.0dBm） | ❌ RF 开关没动 → 查 `0x9D 01` |
| `st=0x52(RX)` / `pkt=0x01(LoRa)` / `err=0` | ✅ 配置生效 |
| `#rx ... data="LRTESTnn"` | ✅ **链路通**，同时给出 rssi/snr |
| 只出现 `CRC error` | ⚠ 收到东西但解不出：同步字/参数不一致、干扰、或太远 |
| `mine: rx=0 crc=0` 且对端在发 | ⚠ 完全没收到：查对端是否真在发（`air≈8ms`）、`0x9D`/`0x08`、天线、频点 |
| `PER` 随距离上涨 | ✅ 正常衰减；`snr` 掉到 0dB 以下就会开始丢包 |

## 命令（行末回车）

| 命令 | 作用 |
| --- | --- |
| `a` | 立刻打印报告 |
| `l` | 每秒报告 开/关 |
| `r` | 重新配置并进接收 |
| `f<MHz>` | 改频点（如 `f470.5`）→ 重新配置 + 进接收 |
| `n` | **底噪扫描** 460~480MHz 每 2MHz 一行 RSSI（判断是宽带噪声还是真实信号） |
| `s` | SPI 时钟换档 1M → 2M → 4M → 8M → 16M |
| `0740` / `0320.16` | 读寄存器（带原始字节回显） |
| `h` | 帮助 |

## 双板测试

```bash
cd firmware/lora-tx && pio run -e gun-s3 -t upload     # 板 A（发，每秒一包）
cd firmware/lora-rx && pio run -e gun-s3 -t upload     # 板 B（收）
```

2026-09-12 实测（两块板贴在一起）：**19/19 收全、0 CRC 错、0 包头错、`air=8.0xms`**，
`rssi=−31dBm / snr=12~13.5dB`，芯片计数与本地计数完全一致。
距离/PER 曲线：把两板拉远（0.1m→1m→5m→10m→30m→100m，视距），每点停 30s 记录
`rssi/snr/PER`；发端按 `p` 调到 200ms 可加快取样。

## 烧录

```bash
cd firmware/lora-rx
pio run -e gun-s3 -t upload      # 枪端 / 网关板都用这个 env
pio device monitor -b 115200
```

## 文件

| 文件 | 内容 |
| --- | --- |
| `platformio.ini` | 单 env `gun-s3`（枪端与网关接线相同）；只用 Arduino 核心 + SPI，无外部库 |
| `src/config.h` | 引脚、射频参数、命令码与寄存器地址 |
| `src/main.cpp` | 配置序列、连续接收与 IRQ 处理、收包打印、PER 统计、底噪扫描、串口命令 |
