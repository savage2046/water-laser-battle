# spi-read —— 第二步：用 SPI 读 LoRa(SX126x) 寄存器，输出到串口

> 第一步 `wire-probe` 确认连线没问题后，这一步**用 SPI 真读芯片**：
> 读版本串、LoRa 同步字（`0x0740`/`0x0741`）、RxGain、trim、错误标志、随机数。
> 开机先给一次复位脉冲，所以读到的应该是**手册的复位默认值** —— 这是"读得对不对"的硬判据。
>
> 2026-09-12 起**枪端与网关接线相同**（都是 ESP32-S3），两块板烧同一个 env。

## 烧录

```bash
cd firmware/spi-read
pio run -e gun-s3 -t upload      # 枪端 / 网关板都用这个 env
pio device monitor -b 115200
```

## 1. 上电输出（纯 ASCII，每 2 秒一次）

```
===== spi-read : read SX126x registers over SPI =====
board : gun-board / gateway（ESP32-S3，接线相同）
spi   : NSS=G16 SCK=G42 MOSI=G15 MISO=G41  mode0  1000kHz
ctrl  : RST=G46 (driven HIGH)  BUSY=G45 (input, waited before every command)
after reset the chip is in STDBY_RC and the registers hold their datasheet
reset values (Table 12-1): 0x0740=0x14 0x0741=0x24 0x08AC=0x94 0x0911=0x05 0x0912=0x05
#reset pulse done (RST low 2ms). BUSY now = 0
#1 @1000kHz  st=0x22(STBY_RC)  pkt=0x00(FSK) [default after reset]  err=0x0000  busyTo=0
   ver   0x0320 = "SX1268 V2F 2F02"   OK (SPI works, chip answered)
   sync  0x0740=14(exp 14) OK  0x0741=24(exp 24) OK  -> 0x1424 (private net, LoRa syncword 0x12)
   gain  0x08AC=94(exp 94) OK  xta 0x0911=05(exp 05) OK  xtb 0x0912=05(exp 05) OK  ocp 0x08E7=38
   rnd   0x0819..1C = 95 4A 7B DD  (proves reads are live data, not a stuck bus)
```

**算读通了的标准**：`ver` 是 `"SX1268"`（或 SX1261/SX1262/LLCC68），且
`sync / gain / xta / xtb` 四项都与手册表 12-1 的复位值一致。
只要版本串读对，就说明 **NSS/SCK/MOSI/MISO 四线 + 供电 + 复位全部正常**。

## 2. 读的是什么（全部按手册 §13 的时序）

⚠️ **最容易踩的坑**：SX126x 返回**数据**的命令，在命令/地址之后**先吐一个 `Status` 字节**，
数据从下一个字节才开始。手册的 SPI 交互表原文（`DS_SX1262.pdf` 第 77 / 83 / 97 页）：

| 命令 | 表号 | byte0 | 1 | 2 | 3 | 4 | 5... |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **ReadRegister** `0x1D` | 表 13-25 | opcode | addr[15:8] | addr[7:0] | **NOP→Status** | **data@addr** | data+1 |
| GetPacketType `0x11` | 表 13-39 | opcode | NOP→Status | **NOP→packetType** | | | |
| GetDeviceErrors `0x17` | 表 13-84 | opcode | NOP→Status | **OpError[15:8]** | **OpError[7:0]** | | |
| GetRssiInst `0x15` | 表 13-82 | opcode | NOP→Status | **RssiInst** | | | |
| GetStatus `0xC0` | 表 13-78 | opcode | **NOP→Status** | | | | |

ReadRegister 一节正文原话：「主机必须在发送两字节的地址后**发送一个 NOP**，
然后可以开始进行下一个 NOP 触发的数据接收。」→ **读 N 字节要发 N+1 个 NOP，数据从 `rx[4]` 取**。

RadioLib 也是这么做的：`Module.cpp:402` 取 `buffIn[cmdLen + widths[STATUS]/8]`，
而 SX126x 的 `widths[STATUS]` 保持默认 `BITS_8`（`Module.h:210`）→ 即 `buffIn[cmdLen+1]`。

| 寄存器 | 含义 | 复位值 |
| --- | --- | --- |
| `0x0320` | 版本串（16B ASCII） | — |
| **`0x0740` / `0x0741`** | **LoRa 同步字 MSB / LSB** | **0x14 / 0x24**（私有网 `0x1424`、公共网 `0x3444`） |
| `0x08AC` | RxGain | 0x94（boosted 0x96） |
| `0x08E7` | OCP 过流保护 | 芯片相关（SX1262/1268 常为 0x38） |
| `0x0911` / `0x0912` | XTA / XTB 内部电容 trim | 0x05 |
| `0x0819..0x081C` | 32 位随机数 | — |

> ⚠️ **频率 / SF / 带宽读不回来**：它们是 `SetRfFrequency(0x86)`、`SetModulationParams(0x8B)`
> 这类**命令**配置的，手册寄存器映射表（表 12-1）里没有对应的可读寄存器。
> 确认对端实际频点只能靠 `lora-gwtest` 扫频，或配置时自己记录。

## 3. 命令（行末回车）

| 命令 | 作用 |
| --- | --- |
| `a` | 立刻打印一次报告 |
| `l` | 自动打印 开/关（默认 ON，每 2 秒一次） |
| `s` | SPI 时钟换档：1M → 2M → 4M → 8M → 16M（**读出乱码时先降到 1M**） |
| `0740` | 读地址 `0x0740` 的 1 字节，**并打印整条事务的原始字节** |
| `0320.16` | 读地址 `0x0320` 起 16 字节 |
| `h` | 帮助 |

`0740` 的输出长这样，可以直接核对偏移（第 4 字节起才是数据）：

```
#read 0x0740 x1 = 14   (.)
#  raw rx: cmd=1D addr=0740 status=A2 | data= 14
```

## 4. 读不出来时怎么判

| 现象 | 结论 |
| --- | --- |
| `ver = "SX1268"` | ✅ SPI 完全正常 |
| `ver` 全 `FF` → `MISO stuck HIGH` | MISO 断线（被上拉）/ 模组没上电 / NSS 没拉低 |
| `ver` 全 `00` → `MISO stuck LOW` | MISO 短路到 GND / 芯片没驱动（在复位中）/ NSS 没选中 |
| 乱码（garbage） | 线太长/接触不良，或 SPI 时钟太快 → 按 `s` 降到 1MHz |
| 每条第 1 字节恒为 `A2`、版本串晚一个字节出现 | **读偏移错了一个字节** → 数据从第 4 字节起（见 §2） |
| `busyTo` 一直涨 | BUSY 线不通：RadioLib 会因此报 `SPI_CMD_TIMEOUT`/`CHIP_NOT_FOUND` |
| `pkt=0x00(FSK)` | 正常：复位后默认 FSK，还没配置成 LoRa |
| 同步字不是 `0x1424` | 芯片被别的东西配过（没复位成功）或读到的是别的值 → 查 MISO/MOSI |

## 5. 硬件事实（查手册得到，第一步也用得上）

- `NRESET`（模组第 4 脚）是**输入 + 内部上拉**（表 8-3：所有模式下都是 `IN PU`，
  PU = 典型 50kΩ 上拉）—— **没有内部下拉**，悬空即为高（不复位）。
- 复位需**至少 50µs 低电平**（一般 100µs）；`VIL_N = 0.2×VBAT` 且 NRESET **参考 VBAT**。
- SPI 是 **Mode 0（CPOL=0/CPHA=0）**、MSB first、最高 **16MHz**；NSS 低开始传输、高时 MISO 高阻。
- `BUSY` 高表示芯片忙，发命令前必须等它变低。

## 6. 文件

| 文件 | 内容 |
| --- | --- |
| `platformio.ini` | 单 env `gun-s3`（枪端与网关接线相同）；只用 Arduino 核心 + SPI，无外部库 |
| `src/config.h` | 引脚、SPI 时钟、复位/BUSY 参数、命令码与寄存器地址 |
| `src/main.cpp` | 复位脉冲、底层事务（BUSY 等待/NSS/transferBytes）、报告与判读、串口命令 |
