"""
Band10m action — tune to 10m band (28.400 MHz).
"""

import os
from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input


class Band10m(ActionBase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "band_10m.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        if event != Input.Key.Events.DOWN:
            return
        self.plugin_base.tci.send("vfo:0,0,28400000;")
