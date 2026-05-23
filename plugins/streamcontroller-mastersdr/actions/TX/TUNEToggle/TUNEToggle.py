"""
TUNEToggle action — toggle antenna tuner transmit.
"""

import os
from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input


class TUNEToggle(ActionBase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._tuning = False

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "tune.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        if event != Input.Key.Events.DOWN:
            return
        self._tuning = not self._tuning
        self.plugin_base.tci.send(f"tune:0,{str(self._tuning).lower()};")
