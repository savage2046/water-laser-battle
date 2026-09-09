# 激光对战（laser tag / 激光对抗）系统光学设计参考

> 调研目的：对照本项目 850/940 双通道光学设计（10cm@10m、20cm@50m→100m），
> 收集其它激光对战/敌我识别系统的光学设计做法。结论先行：
>
> **1. 本项目 940 的"4 mrad、50m 处 20cm 光斑"有军用级先例直接背书**
> （US20070236384：4mrad → 50m 0.2m / 500m 2m，"接近最优"）。
> **2. DIY/商用激光对战主流是"IR LED + 单透镜 + 收光锥匹配"**，与本项目公式同源，
> 但束宽普遍远宽于本项目（靠接收端多传感器命中，不是窄束瞄准）。
> **3. 功率/环境光自适应**（户外升档）是专利级行业做法，印证本项目 4 档电流逻辑。

---

## 1. 专利文献

| 专利 | 系统形态 | 光学要点 | 对本项目的意义 |
| --- | --- | --- | --- |
| [US20070236384（步枪 IFF 敌我识别）](https://www.freepatentsonline.com/y2007/0236384.html) | 光纤 GRIN 透镜 + 1310/1550nm 激光，步枪瞄准镜上收/发同镜 | **"光束要锐，但光斑要足够宽以照亮目标上的接收器"**：4 mrad 束 → 50m 处 0.2m、500m 处 2m 光斑，"接近最优"（太宽→同时照亮多人误判；太窄→近距脱靶漏判）；可选机械变焦把近距光斑放大到 ~1m | **940 目标（20cm@50m=4mrad）与该专利同值**；也提示 100m 处 40cm 已接近"同时照多人"边界，可作狙击/突击玩法区分 |
| [US5904621（Tiger Lazer Pro 9000 等家用激光对战）](https://patents.google.com/patent/US5904621) | 枪：IR LED + **枪口聚焦透镜**；40kHz 载波、~43ms 周期；接收：3×光电二极管覆盖 360° | 经典"LED+单透镜聚焦+周身多传感器"架构 | 验证单透镜方案是行业主流基线；载波 40kHz 与本项目 38/56kHz 同代 |
| [US20170191800（Laser Tag Pro 穿戴式激光对战）](https://www.freepatentsonline.com/y2017/0191800.html) | 头带/背心：3+ 个发射器各覆盖 ~120° 扇区（"lens + IR 光源"），全向接收 | **光传感器测环境光 → 动态调红外发射功率/频率**（室内降、户外升）；功率分档决定作用距离 | 印证"档位电流"是行业做法；可升级为环境光自动升档 |
| （上文中引述的）[US6439892](https://patents.google.com/patent/US6439892) | 步枪激光 0.2mrad → 100m 处 4cm 光斑 | 被 IFF 专利当作**"过窄"反例**（近距脱靶）；<1000nm 可聚焦视网膜的伤害论据 | 提醒：追求"激光感"的毫弧度级束对游戏易脱靶且更危险 |

## 2. DIY / 开源社区

| 来源 | 要点 |
| --- | --- |
| [Open-Laser-Tag GitHub Wiki "The laser LED Optics"](https://github.com/Matthijz98/Open-Laser-Tag/wiki/The-laser-LED-Optics)（镜像自 [lasertagparts.com/mtoptics](http://www.lasertagparts.com/mtoptics.htm)） | 激光对战枪全部是 **IR LED + 透镜**；无透镜可用射程 ~数十米封顶，加镜可到 50~600m+；选镜规则=**镜径罩住 LED 半角锥**，安装距离 ≈ 焦距；例表：TSAL6100（±10°）配 Ø19/f55、Ø25.4/f75、Ø38/f110；TSAL6200（±17°）配 Ø25.4/f42、Ø50.8/f82；"长焦/大镜→更远，但 1 英寸级优质镜+对好焦就很好"；**IR 的焦距与可见光不同，需实测** |
| [engineering.stackexchange #51849 "Lens arrangement for laser tag?"](https://engineering.stackexchange.com/questions/51849/lens-arrangement-for-laser-tag) | 官方回答：用 LED 半角 θ½ 与镜半径算理想距离 r/tan(θ½)，再找 f 最接近的现成镜片——与本项目 §1.1 收光匹配公式一致 |
| [UCF 激光对战毕业设计（LD+非球面准直路线）](https://www.ece.ucf.edu/seniordesign/sp2019su2019/g02/Documents/Summer19_G2_Final_Document.pdf) | 激光二极管 + 非球面镜按发散角选型（LD 窄束路线；眼安全等级另算，本项目已论证不推荐 LD） |

## 3. 商用与中文市场（信息有限）

- [真人CS红外激光发射器（维库等产品页）](https://product.dzsc.com/product/infomation/348519/11199120110425181420018.html)、
  [红外枪瞄激光器](https://www.shfeilijia.com/productinfo/80817.html)：
  940/850nm 发射模组，营销级"射程几十~几百米"，无公开光学细节。
- 中文激光对战设备分两派：**窄束准直 + 接收靶点**（训练/竞技向）与
  **宽束 + 背心多传感器**（娱乐向，室内为主，参考 LaserQuest 等）。
- 深度光学参数（镜片 f/Ø、束宽）多为厂内保密，DIY 与专利才是可复现的参考源。

## 4. 可迁移设计点（结合本项目）

1. **940 的 4mrad 选择有依据**：20cm@50m 正落在军用 IFF"最优束宽"区间；若担心 100m
   处 40cm 同时照多人，可把 940 定位成"狙击"（更窄）而 850 保留宽束档当"常规武器"。
2. **打不中≠加透镜**：行业靠"接收传感器数量 + 束宽 + 功率档"来调命中手感；
   本项目 TSOP 是**指向性**接收头（与商用全向接收相反），所以窄束+对正是特色，
   若实测命中率过低，优先加接收面/放宽束，而不是继续收窄。
3. **环境光自适应功率**（US20170191800）＝现有 4 档电流逻辑的升级方向（可加环境光
   传感器自动升档，替代手动档位）。
4. **眼安全**：850/940 均在视网膜吸收区（<1000nm），专利与文献反复警示——维持
   Class 1 平均功率、禁止 CW 大电流近距直视，与本项目既有结论一致。
5. 社区"LED 半角 ↔ 镜径/f"匹配表与本项目"收光比例 ≈ (Ø/2f·tanθ½)²"互相印证，
   可作为现成镜片快速选型查表。

## 5. 参考链接汇总

- [US20070236384 IFF（4mrad 束宽论述）](https://www.freepatentsonline.com/y2007/0236384.html)
- [US5904621 电子游戏红外发射/接收](https://patents.google.com/patent/US5904621)
- [US20170191800 红外游戏系统（环境光调功率）](https://www.freepatentsonline.com/y2017/0191800.html)
- [Open-Laser-Tag Wiki：激光 LED 光学与透镜表](https://github.com/Matthijz98/Open-Laser-Tag/wiki/The-laser-LED-Optics)
- [lasertagparts.com 透镜选型表（原始出处）](http://www.lasertagparts.com/mtoptics.htm)
- [Engineering SE：laser tag 透镜排布问答](https://engineering.stackexchange.com/questions/51849/lens-arrangement-for-laser-tag)
- [openlasertag.org 发射管光学页（SE 答案引用）](https://openlasertag.org/language/en/optical-emmiter-tube/)
- [UCF 激光对战毕设（LD+非球面）](https://www.ece.ucf.edu/seniordesign/sp2019su2019/g02/Documents/Summer19_G2_Final_Document.pdf)
- [LaserQuest 室内激光对战（历史维基存档）](https://web.archive.org/web/20051215000000/http://en.wikipedia.org/wiki/LaserQuest)
- 中文市场：[真人CS红外激光发射器](https://www.hi1718.com/top-products/1-19218813.html) /
  [维库产品页](https://product.dzsc.com/product/infomation/348519/11199120110425181420018.html) /
  [红外枪瞄激光器](https://www.shfeilijia.com/productinfo/80817.html)
