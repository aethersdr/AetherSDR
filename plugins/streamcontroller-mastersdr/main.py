"""
MasterSDR StreamController Plugin
Controls FlexRadio via MasterSDR's TCI WebSocket server.

Actions:
  TX:        PTT (hold), MOXToggle, TUNEToggle, RFPower, TunePower
  Frequency: TuneUp, TuneDown, BandUp, BandDown, Band160m..Band6m
  Mode:      ModeUSB, ModeLSB, ModeCW, ModeAM, ModeFM, ModeDIGU, ModeDIGL, ModeFT8
  Audio:     MuteToggle, VolumeUp, VolumeDown
  DSP:       NBToggle, NRToggle, ANFToggle, APFToggle, SQLToggle
  Slice:     SplitToggle, LockToggle, RITToggle, XITToggle
  DVK:       Play, Record
"""

import os
import sys

from src.backend.PluginManager.PluginBase import PluginBase
from src.backend.PluginManager.ActionHolder import ActionHolder
from src.backend.DeckManagement.InputIdentifier import Input
from src.backend.PluginManager.ActionInputSupport import ActionInputSupport

from .tci_client import TciClient

# --- Existing actions ---
from .actions.PTT.PTT import PTT
from .actions.Play.Play import Play
from .actions.Record.Record import Record

# --- Frequency ---
from .actions.Frequency.TuneUp.TuneUp import TuneUp
from .actions.Frequency.TuneDown.TuneDown import TuneDown
from .actions.Frequency.BandUp.BandUp import BandUp
from .actions.Frequency.BandDown.BandDown import BandDown
from .actions.Frequency.Band160m.Band160m import Band160m
from .actions.Frequency.Band80m.Band80m import Band80m
from .actions.Frequency.Band60m.Band60m import Band60m
from .actions.Frequency.Band40m.Band40m import Band40m
from .actions.Frequency.Band30m.Band30m import Band30m
from .actions.Frequency.Band20m.Band20m import Band20m
from .actions.Frequency.Band17m.Band17m import Band17m
from .actions.Frequency.Band15m.Band15m import Band15m
from .actions.Frequency.Band12m.Band12m import Band12m
from .actions.Frequency.Band10m.Band10m import Band10m
from .actions.Frequency.Band6m.Band6m import Band6m

# --- Mode ---
from .actions.Mode.ModeUSB.ModeUSB import ModeUSB
from .actions.Mode.ModeLSB.ModeLSB import ModeLSB
from .actions.Mode.ModeCW.ModeCW import ModeCW
from .actions.Mode.ModeAM.ModeAM import ModeAM
from .actions.Mode.ModeFM.ModeFM import ModeFM
from .actions.Mode.ModeDIGU.ModeDIGU import ModeDIGU
from .actions.Mode.ModeDIGL.ModeDIGL import ModeDIGL
from .actions.Mode.ModeFT8.ModeFT8 import ModeFT8

# --- TX ---
from .actions.TX.MOXToggle.MOXToggle import MOXToggle
from .actions.TX.TUNEToggle.TUNEToggle import TUNEToggle
from .actions.TX.RFPower.RFPower import RFPower
from .actions.TX.TunePower.TunePower import TunePower

# --- Audio ---
from .actions.Audio.MuteToggle.MuteToggle import MuteToggle
from .actions.Audio.VolumeUp.VolumeUp import VolumeUp
from .actions.Audio.VolumeDown.VolumeDown import VolumeDown

# --- DSP ---
from .actions.DSP.NBToggle.NBToggle import NBToggle
from .actions.DSP.NRToggle.NRToggle import NRToggle
from .actions.DSP.ANFToggle.ANFToggle import ANFToggle
from .actions.DSP.APFToggle.APFToggle import APFToggle
from .actions.DSP.SQLToggle.SQLToggle import SQLToggle

# --- Slice ---
from .actions.Slice.SplitToggle.SplitToggle import SplitToggle
from .actions.Slice.LockToggle.LockToggle import LockToggle
from .actions.Slice.RITToggle.RITToggle import RITToggle
from .actions.Slice.XITToggle.XITToggle import XITToggle

sys.path.append(os.path.dirname(__file__))

# Key-only support (most actions)
_KEY_ONLY = {
    Input.Key: ActionInputSupport.SUPPORTED,
    Input.Dial: ActionInputSupport.UNSUPPORTED,
    Input.Touchscreen: ActionInputSupport.UNSUPPORTED,
}

# Key + dial support (power sliders)
_KEY_AND_DIAL = {
    Input.Key: ActionInputSupport.SUPPORTED,
    Input.Dial: ActionInputSupport.SUPPORTED,
    Input.Touchscreen: ActionInputSupport.UNSUPPORTED,
}


def _add(plugin, cls, suffix, name, support=None):
    """Helper to register an action with less boilerplate."""
    plugin.add_action_holder(ActionHolder(
        plugin_base=plugin,
        action_base=cls,
        action_id_suffix=suffix,
        action_name=name,
        action_support=support or _KEY_ONLY,
    ))


class MasterSDRPlugin(PluginBase):
    def __init__(self):
        self.PLUGIN_NAME = "MasterSDR"
        self.GITHUB_REPO = "https://github.com/ten9876/MasterSDR"
        super().__init__()

        # Shared TCI client — all actions use this
        self.tci = TciClient()
        self.tci.connect("localhost", 50001)

        # ── TX ──
        _add(self, PTT,        "PTT",        "PTT (Push-to-Talk)")
        _add(self, MOXToggle,  "MOXToggle",  "MOX Toggle")
        _add(self, TUNEToggle, "TUNEToggle", "TUNE Toggle")
        _add(self, RFPower,    "RFPower",    "RF Power",    _KEY_AND_DIAL)
        _add(self, TunePower,  "TunePower",  "Tune Power",  _KEY_AND_DIAL)

        # ── Frequency ──
        _add(self, TuneUp,   "TuneUp",   "Tune Up (+1 kHz)")
        _add(self, TuneDown, "TuneDown", "Tune Down (-1 kHz)")
        _add(self, BandUp,   "BandUp",   "Band Up")
        _add(self, BandDown, "BandDown", "Band Down")
        _add(self, Band160m, "Band160m", "160m Band")
        _add(self, Band80m,  "Band80m",  "80m Band")
        _add(self, Band60m,  "Band60m",  "60m Band")
        _add(self, Band40m,  "Band40m",  "40m Band")
        _add(self, Band30m,  "Band30m",  "30m Band")
        _add(self, Band20m,  "Band20m",  "20m Band")
        _add(self, Band17m,  "Band17m",  "17m Band")
        _add(self, Band15m,  "Band15m",  "15m Band")
        _add(self, Band12m,  "Band12m",  "12m Band")
        _add(self, Band10m,  "Band10m",  "10m Band")
        _add(self, Band6m,   "Band6m",   "6m Band")

        # ── Mode ──
        _add(self, ModeUSB,  "ModeUSB",  "Mode: USB")
        _add(self, ModeLSB,  "ModeLSB",  "Mode: LSB")
        _add(self, ModeCW,   "ModeCW",   "Mode: CW")
        _add(self, ModeAM,   "ModeAM",   "Mode: AM")
        _add(self, ModeFM,   "ModeFM",   "Mode: FM")
        _add(self, ModeDIGU, "ModeDIGU", "Mode: DIGU")
        _add(self, ModeDIGL, "ModeDIGL", "Mode: DIGL")
        _add(self, ModeFT8,  "ModeFT8",  "Mode: FT8 (20m)")

        # ── Audio ──
        _add(self, MuteToggle, "MuteToggle", "Mute Toggle")
        _add(self, VolumeUp,   "VolumeUp",   "Volume Up")
        _add(self, VolumeDown, "VolumeDown", "Volume Down")

        # ── DSP ──
        _add(self, NBToggle,  "NBToggle",  "Noise Blanker Toggle")
        _add(self, NRToggle,  "NRToggle",  "Noise Reduction Toggle")
        _add(self, ANFToggle, "ANFToggle", "Auto Notch Filter Toggle")
        _add(self, APFToggle, "APFToggle", "Audio Peaking Filter Toggle")
        _add(self, SQLToggle, "SQLToggle", "Squelch Toggle")

        # ── Slice ──
        _add(self, SplitToggle, "SplitToggle", "Split Toggle")
        _add(self, LockToggle,  "LockToggle",  "Lock Toggle")
        _add(self, RITToggle,   "RITToggle",   "RIT Toggle")
        _add(self, XITToggle,   "XITToggle",   "XIT Toggle")

        # ── DVK ──
        _add(self, Play,   "Play",   "Play Recording")
        _add(self, Record, "Record", "Record")

        self.register(
            plugin_name="MasterSDR",
            github_repo="https://github.com/ten9876/MasterSDR",
            plugin_version="1.0.0",
            app_version="1.0.0-alpha",
        )
