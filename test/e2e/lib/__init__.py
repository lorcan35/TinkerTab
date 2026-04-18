"""TinkerTab e2e test support library."""

from .tab5 import Tab5Client, Tab5Error
from .touch import Coord, HOME, ORB_CENTER
from .images import (
    assert_color_near,
    assert_amber_glow,
    dominant_color,
    sample_color,
)

__all__ = [
    "Tab5Client",
    "Tab5Error",
    "Coord",
    "HOME",
    "ORB_CENTER",
    "assert_color_near",
    "assert_amber_glow",
    "dominant_color",
    "sample_color",
]
