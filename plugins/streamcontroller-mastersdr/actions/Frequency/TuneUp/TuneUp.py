"""
TuneUp action — increase VFO frequency by one step.
"""

import os
from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input


class TuneUp(ActionBase):
    TUNE_STEP_HZ = 1000  # 1 kHz default step

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._freq_hz = 14_225_000  # default 20m

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "tune_up.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        if event != Input.Key.Events.DOWN:
            return
        self._freq_hz += self.TUNE_STEP_HZ
        self.plugin_base.tci.send(f"vfo:0,0,{self._freq_hz};")
