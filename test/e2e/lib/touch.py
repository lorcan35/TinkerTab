"""Touch coordinate constants for 720×1280 Tab5 display.

Use named coords instead of magic numbers so tests are self-documenting.
Update when the layout changes.
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Coord:
    x: int
    y: int

    def as_tuple(self) -> tuple[int, int]:
        return (self.x, self.y)


# Display dimensions
SW = 720
SH = 1280

# ── Home v5 (ambient) ──
#   These will be used post-overhaul — some may not hit anything on
#   the pre-overhaul tileview home and that's fine (tests drive HOME.
#   explicitly rather than assume).
ORB_CENTER = Coord(SW // 2, 720)        # 500px orb centered at ~48% vertical
HERO_TEXT = Coord(SW // 2, 200)         # "Afternoon" area
STREAM_GUTTER_TOP = Coord(70, 420)
STREAM_GUTTER_MID = Coord(70, 680)
STREAM_GUTTER_BOT = Coord(70, 940)
POEM_AREA = Coord(SW // 2, 1080)
SWIPE_UP_HINT = Coord(SW // 2, 1220)
EDGE_L = Coord(4, SH // 2)
EDGE_R = Coord(SW - 4, SH // 2)

# ── Legacy home (current firmware, pre-overhaul) ──
#   4-tab nav strip at bottom, ~90-180 per tab.
NAV_HOME = Coord(90, 1230)
NAV_NOTES = Coord(270, 1230)
NAV_CHAT = Coord(450, 1230)
NAV_SETTINGS = Coord(630, 1230)

# ── Voice overlay ──
VOICE_CANCEL_SWIPE_FROM = Coord(SW // 2, 900)
VOICE_CANCEL_SWIPE_TO = Coord(SW // 2, 1240)
VOICE_RELEASE = Coord(SW // 2, 1150)

# ── Keyboard (post-rework) ──
#   Key height is DPI_SCALE(52) = ~72px on Tab5, first row starts after
#   preview (60px) + handle padding (~10px) + panel pad (~20px) = ~90px
#   from the keyboard top.  Keyboard sits at bottom, so first row y:
KB_ROW1_Y = SH - 440     # row 0 (q-p) baseline
KB_ROW2_Y = SH - 360     # row 1 (a-l)
KB_ROW3_Y = SH - 280     # row 2 (shift + z-m + bksp)
KB_ROW4_Y = SH - 200     # row 3 (123 + , + space + . + send)


# Convenience aliases
HOME = ORB_CENTER   # "tap HOME" on ambient = tap orb
