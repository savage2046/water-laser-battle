"""IPC-2221 trace current capacity calculator."""
import math

def current(w_mil, oz, dT, inner=False):
    k = 0.024 if inner else 0.048
    A = w_mil * oz * 1.378
    return k * dT**0.44 * A**0.725

def width_for(i, oz=1, dT=10, inner=False):
    k = 0.024 if inner else 0.048
    A = (i / (k * dT**0.44)) ** (1 / 0.725)
    return A / (oz * 1.378)

print("=== 线宽载流 (IPC-2221, 外层, 温升10/20°C) ===")
print(f"{'线宽':>8} {'mm':>6} {'1oz/10C':>9} {'1oz/20C':>9} {'2oz/10C':>9}")
for mil in [8, 10, 12, 15, 20, 30, 50, 100, 150, 200]:
    a = current(mil, 1, 10); b = current(mil, 1, 20); c = current(mil, 2, 10)
    print(f"{mil:>7}mil {mil*0.0254:>6.2f} {a:>8.2f}A {b:>8.2f}A {c:>8.2f}A")

print("\n=== 目标电流所需最小线宽 (1oz外层, 温升10°C) ===")
for i in [1, 2, 3, 5, 8, 10, 15, 20]:
    w = width_for(i)
    print(f"{i:>2}A -> {w:>6.1f} mil = {w*0.0254:>5.1f} mm")

print("\n=== 内层 10mil 对比 ===")
for oz in [1, 2]:
    print(f"10mil 内层 {oz}oz/10C: {current(10, oz, 10, True):.2f}A")

print("\n=== 本项目关键路径建议 ===")
print("电机/采样回路电流(堵转保护阈值内, 假设 5-20A):")
for i, desc in [(5, "持续发射平均"), (10, "峰值/堵转前"), (20, "堵转保护上限")]:
    w = width_for(i, oz=2)  # 推荐 2oz 铜
    print(f"  {desc} {i}A -> 2oz 需 {w:.0f} mil ≈ {w*0.0254:.1f} mm（建议覆铜或加宽）")
