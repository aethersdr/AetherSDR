"""
RITToggle action — toggle Receiver Incremental Tuning.
"""

import os
from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input


class RITToggle(ActionBase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._enabled = False

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "rit.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        if event != Input.Key.Events.DOWN:
            return
        self._enabled = not self._enabled
        self.plugin_base.tci.send(f"rit_enable:0,{str(self._enabled).lower()};")
