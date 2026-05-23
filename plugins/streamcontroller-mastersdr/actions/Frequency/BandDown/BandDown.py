"""
BandDown action — cycle to the next lower amateur band.
"""

import os
from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input

# Band defaults from BandDefs.h (name, freq_hz)
_BANDS = [
    ("160m",  1_900_000),
    ("80m",   3_800_000),
    ("60m",   5_357_000),
    ("40m",   7_200_000),
    ("30m",  10_125_000),
    ("20m",  14_225_000),
    ("17m",  18_130_000),
    ("15m",  21_300_000),
    ("12m",  24_950_000),
    ("10m",  28_400_000),
    ("6m",   50_150_000),
]


class BandDown(ActionBase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "band_down.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        if event != Input.Key.Events.DOWN:
            return
        if not hasattr(self.plugin_base, "_band_index"):
            self.plugin_base._band_index = 5  # default 20m
        self.plugin_base._band_index = (self.plugin_base._band_index - 1) % len(_BANDS)
        _, freq_hz = _BANDS[self.plugin_base._band_index]
        self.plugin_base.tci.send(f"vfo:0,0,{freq_hz};")
