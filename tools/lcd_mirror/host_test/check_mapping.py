import os
"""Windows 侧校验（py -3.10）：鼠标坐标 -> 面板坐标的映射，对着**真实渲染出来的图**验。

  py -3.10 ./check_mapping.py

做法（不靠推导，靠实测）：
  1. 造一张 390x450 的面板图，在网格点上放**颜色唯一**的标记像素（其余全黑）；
  2. 用真客户端里的 render_display()（GUI 用的就是它）旋转 + 缩放；
  3. 反过来扫渲染图的每个像素，用真客户端里的 widget_to_panel() 反算面板坐标，
     跟"这个颜色本来是哪个标记"比。

  因为标记颜色经过 PIL 的 rotate/resize 是一对一搬过去的，所以这等于把
  "窗口像素 <-> 面板像素"整张映射表在 4 个旋转 × 4 个缩放下全查了一遍。

顺带做一次**反向对照**：故意用错的旋转角去算，必须报出大量错误 ——
证明这个测试真的能发现映射错，而不是恒真。
"""
import importlib.util
import sys

from PIL import Image

PANEL_W, PANEL_H = 390, 450
GRID = 7           # 每隔 7 像素放一个标记


def load_client():
    spec = importlib.util.spec_from_file_location(
        "lcd_mirror", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lcd_mirror.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def build_panel():
    """面板图 + 颜色 -> 面板坐标 的对照表。"""
    img = Image.new("RGB", (PANEL_W, PANEL_H), (0, 0, 0))
    lut = {}
    idx = 0
    for y in range(0, PANEL_H, GRID):
        for x in range(0, PANEL_W, GRID):
            idx += 1
            col = (idx // 256, idx % 256, 200)
            assert col not in lut, "标记颜色撞了"
            lut[col] = (x, y)
            img.putpixel((x, y), col)
    return img, lut


def score(mod, img, lut, rotate, scale, use_rotate):
    disp = mod.render_display(img, rotate, scale)
    checked = 0
    bad = 0
    worst = 0
    examples = []
    for dy in range(disp.height):
        for dx in range(disp.width):
            col = disp.getpixel((dx, dy))
            if col not in lut:
                continue
            want = lut[col]
            got = mod.widget_to_panel(dx, dy, PANEL_W, PANEL_H,
                                      use_rotate, scale)
            checked += 1
            err = max(abs(got[0] - want[0]), abs(got[1] - want[1]))
            if err:
                bad += 1
                worst = max(worst, err)
                if len(examples) < 3:
                    examples.append(((dx, dy), got, want))
    return checked, bad, worst, examples


def main():
    mod = load_client()
    img, lut = build_panel()
    print(f"标记点 {len(lut)} 个（每 {GRID} 像素一个）")

    ok = True
    for rotate in (0, 90, 180, 270):
        for scale in (1.0, 1.5, 2.0, 0.7):
            checked, bad, worst, ex = score(mod, img, lut, rotate, scale, rotate)
            flag = "OK " if worst <= 1 else "FAIL"
            print(f"  {flag} rotate={rotate:3d} scale={scale:<4} "
                  f"查了 {checked:6d} 个窗口像素 错 {bad:5d} 个 "
                  f"最大偏差 {worst} 像素")
            if worst > 1:
                ok = False
                print("       例:", ex)
            if scale == 1.0 and bad != 0:
                ok = False
                print("       ⚠ scale=1.0 必须一像素都不差")

    # 反向对照：故意用错的旋转角，必须报出一大堆错
    print("\n反向对照（故意用 rotate=0 去算 rotate=90/180/270 的画面）：")
    for rotate in (90, 180, 270):
        checked, bad, worst, _ = score(mod, img, lut, rotate, 1.0, 0)
        print(f"  rotate 真={rotate:3d} 按 0 算 -> 错 {bad}/{checked}，"
              f"最大偏差 {worst} 像素")
        if bad < checked * 0.2:
            ok = False
            print("       ⚠ 对照不敏感：错的旋转角居然没报出一堆错")

    if not ok:
        sys.exit("FAIL: 坐标映射没通过")

    print("\nPASS: 4 个旋转 × 4 个缩放，窗口坐标 -> 面板坐标最大偏差 <= 1 像素"
          "（scale=1.0 时 0 偏差）；反向对照能报错")


if __name__ == "__main__":
    main()
