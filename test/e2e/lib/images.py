"""Pixel sampling + color assertions.

Keep it simple — we're not doing OCR or structural diffs. We sample
regions, check dominance or nearness to expected colors, and fail with
a useful message.
"""
from __future__ import annotations

from collections import Counter
from typing import Tuple

from PIL import Image

Color = Tuple[int, int, int]

# Brand constants (device-matching)
AMBER = (245, 158, 11)
AMBER_HOT = (255, 182, 55)
AMBER_DEEP = (122, 74, 6)
BG = (8, 8, 14)
ROSE = (244, 63, 94)
EMERALD = (34, 197, 94)
AZURE = (59, 130, 246)
WHITE_ISH = (232, 232, 239)


def sample_color(img: Image.Image, x: int, y: int, window: int = 3) -> Color:
    """Average color in a (2*window+1)² pixel window around (x,y)."""
    x0, y0 = max(0, x - window), max(0, y - window)
    x1, y1 = min(img.width, x + window + 1), min(img.height, y + window + 1)
    if x1 <= x0 or y1 <= y0:
        raise ValueError(f"bad coord ({x},{y}) for image {img.size}")
    crop = img.crop((x0, y0, x1, y1))
    # Average
    r_tot = g_tot = b_tot = 0
    n = 0
    for r, g, b in crop.getdata():
        r_tot += r
        g_tot += g
        b_tot += b
        n += 1
    return (r_tot // n, g_tot // n, b_tot // n)


def _color_distance(a: Color, b: Color) -> float:
    # Euclidean distance in RGB — good enough for our crude assertions.
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2) ** 0.5


def assert_color_near(
    actual: Color,
    expected: Color,
    tolerance: float = 40.0,
    label: str = "",
) -> None:
    d = _color_distance(actual, expected)
    if d > tolerance:
        prefix = f"[{label}] " if label else ""
        raise AssertionError(f"{prefix}color mismatch: actual={actual} expected={expected} distance={d:.1f} > tolerance={tolerance}")


def assert_amber_glow(
    img: Image.Image,
    cx: int,
    cy: int,
    radius: int = 100,
    min_amber_ratio: float = 0.15,
) -> None:
    """Assert at least `min_amber_ratio` of pixels in a circle around (cx,cy) are amber-ish.

    "Amber-ish" = distance < 80 from AMBER in RGB.  This is forgiving enough
    for gradients and shadows without catching pure black.
    """
    box = img.crop((cx - radius, cy - radius, cx + radius, cy + radius))
    total = 0
    amber_like = 0
    for r, g, b in box.getdata():
        total += 1
        if _color_distance((r, g, b), AMBER) < 80:
            amber_like += 1
    if total == 0:
        raise AssertionError("empty crop region")
    ratio = amber_like / total
    if ratio < min_amber_ratio:
        raise AssertionError(
            f"expected amber ratio >= {min_amber_ratio:.2f} around ({cx},{cy}) r={radius}, "
            f"got {ratio:.2f} ({amber_like}/{total})"
        )


def dominant_color(img: Image.Image, box: tuple[int, int, int, int] | None = None, n: int = 1) -> list[Color]:
    """Return the N most common colors in `box` (or the whole image).

    Quantizes to reduce noise (shift right by 3 bits per channel).
    """
    region = img.crop(box) if box else img
    c: Counter = Counter()
    for r, g, b in region.getdata():
        c[(r >> 3 << 3, g >> 3 << 3, b >> 3 << 3)] += 1
    return [rgb for rgb, _ in c.most_common(n)]
