"""
Band15m action — tune to 15m band (21.300 MHz).
"""

import os
from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input


class Band15m(ActionBase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "band_15m.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        if event != Input.Key.Events.DOWN:
            return
        self.plugin_base.tci.send("vfo:0,0,21300000;")
