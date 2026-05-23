"""
ModeFT8 action — switch to DIGU mode and tune to 20m FT8 frequency (14.074 MHz).
"""

import os
from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input


class ModeFT8(ActionBase):
    FT8_FREQ_HZ = 14_074_000  # 20m FT8 dial frequency

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "mode_ft8.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        if event != Input.Key.Events.DOWN:
            return
        tci = self.plugin_base.tci
        tci.send("modulation:0,DIGU;")
        tci.send(f"vfo:0,0,{self.FT8_FREQ_HZ};")
