#!/usr/bin/env python3
"""Draw the toolbar icons for burned-in subtitle extraction, soft subtitle
export and the MCP server, in the flat style of the dub buttons.

Run from the repository root:

    python tools/generate_button_icons.py

Needs Pillow. Each icon is drawn large and scaled down to every size.
"""

import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = (16, 24, 32, 48, 64)
CANVAS = 256

# The colours of the dub buttons
RED = (226, 24, 48, 255)
BLUE = (24, 92, 214, 255)
DARK = (40, 44, 60, 255)
SCREEN = (64, 70, 92, 255)
WHITE = (255, 255, 255, 255)


def screen(d, box):
    """A video frame"""
    d.rounded_rectangle(box, radius=18, fill=SCREEN, outline=DARK, width=12)


def text_lines(d, x0, x1, y, colour, width=14, gap=26):
    """Two centred lines of subtitle text, the second shorter"""
    d.line((x0, y, x1, y), fill=colour, width=width)
    inset = (x1 - x0) // 5
    d.line((x0 + inset, y + gap, x1 - inset, y + gap), fill=colour, width=width)


def arrow(d, x, y0, y1, colour, width=22, head=34):
    """A downward arrow from y0 to y1"""
    d.line((x, y0, x, y1 - head // 2), fill=colour, width=width)
    d.polygon([(x - head, y1 - head), (x + head, y1 - head), (x, y1 + 4)], fill=colour)


def extract_icon():
    # Text lifted off the bottom of the picture
    img = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    screen(d, (12, 16, 244, 172))
    text_lines(d, 56, 200, 116, RED)
    arrow(d, 128, 150, 236, BLUE)
    return img


def export_icon():
    # A picture whose subtitle track is a separate, switchable layer
    img = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    screen(d, (12, 16, 244, 172))
    d.polygon([(108, 54), (108, 124), (164, 89)], fill=RED)
    d.rounded_rectangle((40, 160, 216, 240), radius=22, fill=BLUE, outline=DARK, width=10)
    text_lines(d, 78, 178, 186, WHITE, width=12, gap=28)
    return img


def mcp_icon():
    # An agent's head, plugged in
    img = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.line((128, 30, 128, 64), fill=DARK, width=12)
    d.ellipse((108, 8, 148, 48), fill=RED, outline=DARK, width=8)
    d.rounded_rectangle((34, 62, 222, 214), radius=34, fill=SCREEN, outline=DARK, width=12)
    for cx in (92, 164):
        d.ellipse((cx - 24, 104, cx + 24, 152), fill=BLUE, outline=WHITE, width=8)
    d.rounded_rectangle((90, 172, 166, 188), radius=8, fill=WHITE)
    d.rounded_rectangle((10, 112, 34, 164), radius=8, fill=DARK)
    d.rounded_rectangle((222, 112, 246, 164), radius=8, fill=DARK)
    return img


def main():
    out_dir = os.path.join(ROOT, "src", "bitmaps", "button")
    for name, draw in (("hardsub_extract_button", extract_icon),
                       ("soft_subs_export_button", export_icon),
                       ("mcp_server_button", mcp_icon)):
        big = draw()
        for size in SIZES:
            big.resize((size, size), Image.LANCZOS).save(os.path.join(out_dir, f"{name}_{size}.png"))


if __name__ == "__main__":
    main()
