#!/usr/bin/env python3
"""
Generate PNG icon assets for MasterSDR StreamController plugin.
Renders text-on-dark-background icons at 72x72 for StreamDeck LCD displays.

Usage: python gen_icons.py
Output: assets/*.png
"""

import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("Pillow required: pip install Pillow")
    sys.exit(1)

SIZE = 72
BG_COLOR = (15, 15, 26)       # #0f0f1a — MasterSDR theme
TEXT_COLOR = (255, 255, 255)   # white
ACCENT_COLOR = (0, 220, 255)  # cyan accent

ASSETS_DIR = os.path.join(os.path.dirname(__file__), "assets")

# (filename, label_lines, use_accent)
ICONS = [
    # TX
    ("ptt.png",        ["PTT"],        True),
    ("mox.png",        ["MOX"],        True),
    ("tune.png",       ["TUNE"],       True),
    ("rf_power.png",   ["RF", "PWR"],  False),
    ("tune_power.png", ["TUNE", "PWR"], False),
    # Frequency
    ("tune_up.png",    ["\u25b2", "FREQ"], False),
    ("tune_down.png",  ["\u25bc", "FREQ"], False),
    ("band_up.png",    ["BAND", "\u25b2"], False),
    ("band_down.png",  ["BAND", "\u25bc"], False),
    # Band buttons
    ("band_160m.png",  ["160m"],       False),
    ("band_80m.png",   ["80m"],        False),
    ("band_60m.png",   ["60m"],        False),
    ("band_40m.png",   ["40m"],        False),
    ("band_30m.png",   ["30m"],        False),
    ("band_20m.png",   ["20m"],        False),
    ("band_17m.png",   ["17m"],        False),
    ("band_15m.png",   ["15m"],        False),
    ("band_12m.png",   ["12m"],        False),
    ("band_10m.png",   ["10m"],        False),
    ("band_6m.png",    ["6m"],         False),
    # Mode buttons
    ("mode_usb.png",   ["USB"],        False),
    ("mode_lsb.png",   ["LSB"],        False),
    ("mode_cw.png",    ["CW"],         True),
    ("mode_am.png",    ["AM"],         False),
    ("mode_fm.png",    ["FM"],         False),
    ("mode_digu.png",  ["DIGU"],       False),
    ("mode_digl.png",  ["DIGL"],       False),
    ("mode_ft8.png",   ["FT8"],        True),
    # Audio
    ("mute.png",       ["MUTE"],       True),
    ("volume_up.png",  ["VOL", "\u25b2"], False),
    ("volume_down.png",["VOL", "\u25bc"], False),
    # DSP
    ("nb.png",         ["NB"],         False),
    ("nr.png",         ["NR"],         False),
    ("anf.png",        ["ANF"],        False),
    ("apf.png",        ["APF"],        False),
    ("sql.png",        ["SQL"],        False),
    # Slice
    ("split.png",      ["SPLIT"],      False),
    ("lock.png",       ["LOCK"],       False),
    ("rit.png",        ["RIT"],        False),
    ("xit.png",        ["XIT"],        False),
    # DVK
    ("record.png",     ["\u25cf", "REC"], True),
    ("play.png",       ["\u25b6", "PLAY"], False),
]


def make_icon(filename, lines, use_accent):
    img = Image.new("RGB", (SIZE, SIZE), BG_COLOR)
    draw = ImageDraw.Draw(img)
    color = ACCENT_COLOR if use_accent else TEXT_COLOR

    # Try to use a built-in font; fall back to default
    try:
        if len(lines) == 1 and len(lines[0]) <= 3:
            font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 24)
        else:
            font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 16)
    except (OSError, IOError):
        font = ImageFont.load_default()

    total_height = sum(draw.textbbox((0, 0), line, font=font)[3] - draw.textbbox((0, 0), line, font=font)[1] for line in lines)
    spacing = 2
    total_height += spacing * (len(lines) - 1)
    y = (SIZE - total_height) // 2

    for line in lines:
        bbox = draw.textbbox((0, 0), line, font=font)
        w = bbox[2] - bbox[0]
        h = bbox[3] - bbox[1]
        x = (SIZE - w) // 2
        draw.text((x, y), line, fill=color, font=font)
        y += h + spacing

    img.save(os.path.join(ASSETS_DIR, filename))


def main():
    os.makedirs(ASSETS_DIR, exist_ok=True)
    for filename, lines, accent in ICONS:
        make_icon(filename, lines, accent)
        print(f"  {filename}")
    print(f"\nGenerated {len(ICONS)} icons in {ASSETS_DIR}/")


if __name__ == "__main__":
    main()
