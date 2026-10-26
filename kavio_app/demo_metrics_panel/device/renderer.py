"""无第三方依赖的 BGRA8888 指标矩形绘制器。"""

from __future__ import annotations

from typing import Iterable

# Surface 尺寸与 Host Component 提供的 App Panel 中的 kBoxWidth/kBoxHeight 对应，便于直观看到位置同步。
# 560x220 取自 Windows Host 面板中的指标盒尺寸；宽高变化须两端同步。
# 每行宽度 4 字节、字形 2 倍缩放仅是演示排版参数。
WIDTH = 560
HEIGHT = 220
STRIDE = WIDTH * 4
FONT_SCALE = 2

# 5x7 字体。Demo 只需要大写字母、数字和少量符号。
_GLYPHS = {
    "0": (14, 17, 19, 21, 25, 17, 14), "1": (4, 12, 4, 4, 4, 4, 14),
    "2": (14, 17, 1, 2, 4, 8, 31), "3": (30, 1, 1, 14, 1, 1, 30),
    "4": (2, 6, 10, 18, 31, 2, 2), "5": (31, 16, 16, 30, 1, 1, 30),
    "6": (14, 16, 16, 30, 17, 17, 14), "7": (31, 1, 2, 4, 8, 8, 8),
    "8": (14, 17, 17, 14, 17, 17, 14), "9": (14, 17, 17, 15, 1, 1, 14),
    "A": (14, 17, 17, 31, 17, 17, 17), "B": (30, 17, 17, 30, 17, 17, 30),
    "C": (14, 17, 16, 16, 16, 17, 14), "D": (30, 17, 17, 17, 17, 17, 30),
    "E": (31, 16, 16, 30, 16, 16, 31), "F": (31, 16, 16, 30, 16, 16, 16),
    "G": (14, 17, 16, 23, 17, 17, 15), "H": (17, 17, 17, 31, 17, 17, 17),
    "I": (14, 4, 4, 4, 4, 4, 14), "K": (17, 18, 20, 24, 20, 18, 17),
    "L": (16, 16, 16, 16, 16, 16, 31), "M": (17, 27, 21, 21, 17, 17, 17),
    "N": (17, 25, 21, 19, 17, 17, 17), "O": (14, 17, 17, 17, 17, 17, 14),
    "P": (30, 17, 17, 30, 16, 16, 16), "R": (30, 17, 17, 30, 20, 18, 17),
    "S": (15, 16, 16, 14, 1, 1, 30), "T": (31, 4, 4, 4, 4, 4, 4),
    "U": (17, 17, 17, 17, 17, 17, 14), "V": (17, 17, 17, 17, 17, 10, 4),
    "W": (17, 17, 17, 21, 21, 21, 10), "Y": (17, 17, 10, 4, 4, 4, 4),
    ":": (0, 4, 4, 0, 4, 4, 0), ".": (0, 0, 0, 0, 0, 4, 4),
    "/": (1, 2, 2, 4, 8, 8, 16), "%": (17, 2, 4, 8, 16, 17, 0),
    "-": (0, 0, 0, 31, 0, 0, 0), "_": (0, 0, 0, 0, 0, 0, 31),
    " ": (0, 0, 0, 0, 0, 0, 0),
}


# color 按 BGRA 顺序传入；这里不使用 Pillow/OpenCV，故所有像素操作都直接写 bytearray。
def _put_pixel(image: bytearray, x: int, y: int, color: tuple[int, int, int, int]) -> None:
    if x < 0 or y < 0 or x >= WIDTH or y >= HEIGHT:
        return
    offset = y * STRIDE + x * 4
    image[offset:offset + 4] = bytes(color)


def _fill_rect(image: bytearray, x: int, y: int, width: int, height: int,
               color: tuple[int, int, int, int]) -> None:
    left, top = max(0, x), max(0, y)
    right, bottom = min(WIDTH, x + width), min(HEIGHT, y + height)
    pixel = bytes(color)
    row = pixel * max(0, right - left)
    for yy in range(top, bottom):
        offset = yy * STRIDE + left * 4
        image[offset:offset + len(row)] = row


def _draw_text(image: bytearray, x: int, y: int, text: str) -> None:
    for raw in text.upper():
        glyph = _GLYPHS.get(raw, _GLYPHS[" "])
        for row_index, bits in enumerate(glyph):
            for column in range(5):
                if bits & (1 << (4 - column)):
                    _fill_rect(
                        image,
                        x + column * FONT_SCALE,
                        y + row_index * FONT_SCALE,
                        FONT_SCALE,
                        FONT_SCALE,
                        (255, 255, 255, 255),
                    )
        x += 6 * FONT_SCALE


# 返回值正好是一帧 WIDTH*HEIGHT*4 的连续 BGRA 数据，可直接交给 KavioBridge.write_frame。
# 纯 CPU 画布每次返回完整 BGRA 数据；调用方按自身业务更新频率决定何时写 TX。
def render(lines: Iterable[str]) -> bytes:
    image = bytearray(STRIDE * HEIGHT)
    # 整个 TX Surface 就是可拖动矩形；背景略透明，边框不透明。
    _fill_rect(image, 0, 0, WIDTH, HEIGHT, (24, 24, 24, 210))
    _fill_rect(image, 0, 0, WIDTH, 3, (255, 255, 255, 255))
    _fill_rect(image, 0, HEIGHT - 3, WIDTH, 3, (255, 255, 255, 255))
    _fill_rect(image, 0, 0, 3, HEIGHT, (255, 255, 255, 255))
    _fill_rect(image, WIDTH - 3, 0, 3, HEIGHT, (255, 255, 255, 255))

    y = 14
    for line in lines:
        _draw_text(image, 16, y, line[:43])
        y += 24
        if y + 14 >= HEIGHT:
            break
    return bytes(image)
