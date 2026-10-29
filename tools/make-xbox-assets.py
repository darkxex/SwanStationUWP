#!/usr/bin/env python3
"""SwanStation Xbox - the package's tile and splash images, made from the project's icon and launch background.

    python tools/make-xbox-assets.py     -> xbox/Assets/*.png

SPDX-License-Identifier: GPL-3.0-or-later
"""
import os
from PIL import Image

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
icon = Image.open(os.path.join(root, "xbox", "Branding", "icon.png")).convert("RGBA")
banner = Image.open(os.path.join(root, "xbox", "Branding", "launch-background.png")).convert("RGB")
out = os.path.join(root, "xbox", "Assets")
os.makedirs(out, exist_ok=True)


def square(name, size):
    icon.resize((size, size), Image.LANCZOS).save(os.path.join(out, name))


def wide(name, w, h):
    """The banner cropped to the shape of the tile."""
    bw, bh = banner.size
    scale = max(w / bw, h / bh)
    resized = banner.resize((round(bw * scale), round(bh * scale)), Image.LANCZOS)
    left, top = (resized.width - w) // 2, (resized.height - h) // 2
    resized.crop((left, top, left + w, top + h)).save(os.path.join(out, name))


square("StoreLogo.png", 50)
square("Square44x44Logo.png", 44)
square("Square71x71Logo.png", 71)
square("Square150x150Logo.png", 150)
square("Square310x310Logo.png", 310)
wide("Wide310x150Logo.png", 310, 150)
wide("SplashScreen.png", 620, 300)
print("assets written to", out)
