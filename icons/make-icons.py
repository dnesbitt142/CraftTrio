#!/usr/bin/env python3
"""Generate the suite's original 16-color OS/2 icons with no dependencies."""

from pathlib import Path
from struct import pack


SIZE = 32
COLORS = (
    0x000000, 0x142534, 0x465862, 0x93A6AE,
    0xC6D4D9, 0xE8F0F0, 0xFFFFFF, 0xDDBE9E,
    0x203C76, 0x356CC0, 0xA7C9F0, 0x773527,
    0xD06837, 0xF5B271, 0x23634E, 0x8DD1A7,
)


def inside_polygon(x, y, corners):
    inside = False
    for (x0, y0), (x1, y1) in zip(corners, (*corners[1:], corners[0])):
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            inside = not inside
    return inside


class Icon:
    def __init__(self):
        self.pixels = [[0] * SIZE for _ in range(SIZE)]

    def polygon(self, color, *corners):
        for y, row in enumerate(self.pixels):
            for x in range(SIZE):
                if inside_polygon(x + 0.5, y + 0.5, corners):
                    row[x] = color

    def rect(self, color, left, top, right, bottom):
        for row in self.pixels[top:bottom]:
            row[left:right] = [color] * (right - left)

    def rows(self, size):
        return [[self.pixels[y * SIZE // size][x * SIZE // size] for x in range(size)]
                for y in range(size)]


def word_icon():
    icon = Icon()
    icon.polygon(8, (8, 7), (24, 7), (29, 12), (29, 30), (8, 30))
    icon.polygon(1, (4, 2), (21, 2), (27, 8), (27, 28), (4, 28))
    icon.polygon(6, (5, 3), (20, 3), (26, 9), (26, 27), (5, 27))
    icon.polygon(3, (20, 3), (26, 9), (20, 9))
    icon.rect(8, 7, 6, 18, 12)
    icon.rect(9, 8, 6, 18, 8)
    icon.rect(4, 7, 14, 23, 16)
    icon.rect(2, 7, 18, 22, 19)
    icon.rect(2, 7, 21, 23, 22)
    icon.rect(2, 7, 24, 17, 25)
    return icon


def deck_icon():
    icon = Icon()
    icon.rect(11, 6, 7, 31, 25)
    icon.rect(12, 7, 9, 30, 24)
    icon.rect(1, 2, 4, 28, 22)
    icon.rect(6, 3, 5, 27, 21)
    icon.rect(12, 4, 6, 26, 8)
    icon.rect(5, 4, 9, 26, 20)
    icon.polygon(7, (6, 19), (13, 11), (22, 19))
    icon.polygon(12, (12, 19), (20, 10), (25, 16), (25, 19))
    icon.polygon(13, (17, 13), (20, 10), (24, 15), (20, 14))
    icon.rect(11, 13, 22, 17, 28)
    icon.rect(11, 8, 28, 22, 30)
    icon.rect(12, 10, 28, 21, 29)
    return icon


def grid_icon():
    icon = Icon()
    icon.rect(14, 6, 6, 30, 30)
    icon.rect(1, 3, 3, 28, 28)
    icon.rect(6, 4, 4, 27, 27)
    icon.rect(14, 4, 4, 27, 10)
    icon.rect(15, 6, 6, 12, 8)
    icon.rect(15, 14, 6, 18, 8)
    icon.rect(15, 20, 6, 25, 8)
    icon.rect(5, 4, 10, 10, 27)
    for x in (10, 16, 22):
        icon.rect(3, x, 10, x + 1, 27)
    for y in (14, 18, 22):
        icon.rect(3, 4, y, 27, y + 1)
    icon.rect(14, 16, 18, 23, 23)
    icon.rect(15, 17, 19, 22, 22)
    return icon


def bitmap_header(offset, width, height, depth):
    return pack('<2sIhhIIHHHH', b'CI', 26, 0, 0, offset, 12, width, height, 1, depth)


def bitmap_rows(rows, depth):
    width = len(rows[0])
    stride = ((width * depth + 31) // 32) * 4
    out = bytearray()
    for row in reversed(rows):
        bits = 0
        for index in row:
            bits = (bits << depth) | index
        out.extend((bits << (stride * 8 - width * depth)).to_bytes(stride, 'big'))
    return bytes(out)


def encode_icon(icon):
    sizes = ((32, 0, 0), (40, 1024, 768))
    palette = b''.join(bytes((color & 255, (color >> 8) & 255, color >> 16)) for color in COLORS)
    image_header_size = 14 + 26 + 6 + 26 + len(palette)
    headers = bytearray()
    pixels = bytearray()
    for index, (size, screen_width, screen_height) in enumerate(sizes):
        rows = icon.rows(size)
        xor_mask = bitmap_rows([[0] * size] * size, 1)
        and_mask = bitmap_rows([[int(not pixel) for pixel in row] for row in rows], 1)
        offset = len(sizes) * image_header_size + len(pixels)
        next_header = (index + 1) * image_header_size if index + 1 < len(sizes) else 0
        headers.extend(pack('<2sIIHH', b'BA', 40, next_header, screen_width, screen_height))
        headers.extend(bitmap_header(offset, size, 2 * size, 1))
        headers.extend(bytes((0, 0, 0, 255, 255, 255)))
        headers.extend(bitmap_header(offset + len(xor_mask) + len(and_mask), size, size, 4))
        headers.extend(palette)
        pixels.extend(xor_mask + and_mask + bitmap_rows(rows, 4))
    return headers + pixels


if __name__ == '__main__':
    directory = Path(__file__).resolve().parent
    for name, create in (('WORDCRFT', word_icon), ('DECKCRFT', deck_icon), ('GRIDCRFT', grid_icon)):
        (directory / f'{name}.ICO').write_bytes(encode_icon(create()))
