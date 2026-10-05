#!/usr/bin/env python3
"""Regenerate every icon and branding image from docs/art-sources/aegidubLogo.png.

Run from the repository root after changing the logo:

    python tools/generate_logo_assets.py

Needs Pillow. The wordmark uses Segoe UI Bold on Windows, falling back to
DejaVu Sans Bold elsewhere.
"""

import base64
import io
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "docs", "art-sources", "aegidubLogo.png")

# Colours taken from the logo: the red facets and the blue iris
RED = (226, 24, 48)
BLUE = (24, 92, 214)
OUTLINE = (12, 14, 30)


def path(*parts):
    return os.path.join(ROOT, *parts)


def load_logo():
    logo = Image.open(SOURCE).convert("RGBA")
    # Trim the transparent margin, then centre on a square canvas
    logo = logo.crop(logo.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox())
    side = max(logo.size)
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.paste(logo, ((side - logo.width) // 2, (side - logo.height) // 2))
    return square


def icon(logo, size):
    img = logo.resize((size, size), Image.LANCZOS)
    if size <= 32:
        # Small sizes lose the outlines to resampling blur
        img = img.filter(ImageFilter.UnsharpMask(radius=0.6, percent=80, threshold=1))
    return img


def font(size):
    for name in ("segoeuib.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"):
        for folder in (os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts"),
                       "/usr/share/fonts/truetype/dejavu", ""):
            try:
                return ImageFont.truetype(os.path.join(folder, name), size)
            except OSError:
                pass
    sys.exit("No bold TrueType font found for the wordmark")


def wordmark(height):
    """The name "aegidub" in the logo's colours with a dark outline."""
    f = font(height)
    stroke = max(1, height // 14)
    probe = ImageDraw.Draw(Image.new("RGBA", (1, 1)))
    aegi_w = probe.textlength("aegi", font=f)
    dub_w = probe.textlength("dub", font=f)
    bbox = probe.textbbox((0, 0), "aegidub", font=f, stroke_width=stroke)
    img = Image.new("RGBA", (int(aegi_w + dub_w) + stroke * 2 + 2, bbox[3] + stroke + 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.text((stroke, 0), "aegi", font=f, fill=RED, stroke_width=stroke, stroke_fill=OUTLINE)
    d.text((stroke + aegi_w, 0), "dub", font=f, fill=BLUE, stroke_width=stroke, stroke_fill=OUTLINE)
    return img.crop(img.getbbox())


def faded(logo, size, opacity):
    img = logo.resize((size, size), Image.LANCZOS)
    alpha = img.getchannel("A").point(lambda a: int(a * opacity))
    img.putalpha(alpha)
    return img


def banner(logo, width, height, mark_width, logo_size, logo_pos, mark_pos="top"):
    """White background, a faded logo behind, the wordmark on top."""
    img = Image.new("RGBA", (width, height), (255, 255, 255, 255))
    img.alpha_composite(faded(logo, logo_size, 0.28), logo_pos)
    mark = wordmark(200)
    mark = mark.resize((mark_width, round(mark.height * mark_width / mark.width)), Image.LANCZOS)
    x = (width - mark.width) // 2
    y = round(height * 0.08) if mark_pos == "top" else (height - mark.height) // 2
    img.alpha_composite(mark, (x, y))
    return img.convert("RGB")


def small_installer_image(logo, size):
    img = Image.new("RGBA", size, (255, 255, 255, 255))
    side = min(size) - 4
    mark = icon(logo, side)
    img.alpha_composite(mark, ((size[0] - side) // 2, (size[1] - side) // 2))
    return img.convert("RGB")


def save(img, *parts, **kwargs):
    target = path(*parts)
    img.save(target, **kwargs)
    print("wrote", os.path.relpath(target, ROOT))


def main():
    logo = load_logo()
    ico_sizes = [(s, s) for s in (16, 24, 32, 48, 64, 128, 256)]

    # Windows executable and window icons
    save(icon(logo, 256), "src", "bitmaps", "windows", "icon.ico", sizes=ico_sizes)
    save(icon(logo, 256), "packages", "win_installer", "portable", "icon.ico", sizes=ico_sizes)
    save(icon(logo, 48), "src", "bitmaps", "misc", "wxicon.png")

    # About dialog
    save(banner(logo, 420, 240, 330, 300, (150, 40), "center"), "src", "bitmaps", "misc", "splash.png")

    # Installer artwork
    save(banner(logo, 164, 314, 140, 260, (10, 110)), "packages", "win_installer", "welcome.bmp")
    save(banner(logo, 219, 386, 190, 340, (12, 140)), "packages", "win_installer", "welcome-large.bmp")
    save(banner(logo, 93, 302, 80, 150, (-20, 170)), "packages", "win_installer", "portable", "side-logo.bmp")
    save(small_installer_image(logo, (55, 58)), "packages", "win_installer", "aegisub.bmp")
    save(small_installer_image(logo, (73, 71)), "packages", "win_installer", "aegisub-large.bmp")

    # Linux desktop icons
    for s in (16, 22, 24, 32, 48, 64):
        img = icon(logo, s)
        save(img, "packages", "desktop", f"{s}x{s}.png")
        save(img, "packages", "desktop", f"{s}x{s}", "aegisub.png")
    buf = io.BytesIO()
    icon(logo, 512).save(buf, "PNG")
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
           'width="512" height="512" viewBox="0 0 512 512">'
           f'<image width="512" height="512" xlink:href="data:image/png;base64,{base64.b64encode(buf.getvalue()).decode()}"/>'
           '</svg>\n')
    for target in (("packages", "desktop", "scalable.svg"), ("packages", "desktop", "scalable", "aegisub.svg")):
        with open(path(*target), "w", encoding="utf-8") as f:
            f.write(svg)
        print("wrote", os.path.join(*target))

    # macOS application icon
    save(icon(logo, 1024), "packages", "osx_bundle", "Contents", "Resources", "Aegisub.icns")


if __name__ == "__main__":
    main()
