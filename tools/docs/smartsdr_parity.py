#!/usr/bin/env python3
"""SmartSDR for Mac action parity: every entry of SmartSDR for Mac's MIDI /
Stream Deck action list (185 buttons, 16 wheel functions) mapped to the
AetherSDR keyboard action and MIDI control that do the same thing, or to the
reason there is none.

Usage:
    python3 tools/docs/smartsdr_parity.py --check   # every mapped id is in the
                                                    # generated reference pages
    python3 tools/docs/smartsdr_parity.py --table   # the Markdown table in
                                                    # docs/user/docs/midi-controller-mapping.md

The ids are checked against docs/user/docs/generated/default-shortcuts.md and
midi-actions.md, which tools/docs/gen_reference.py writes from the source, so
a renamed or removed action fails --check by name. Stdlib only.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GENERATED = REPO / "docs" / "user" / "docs" / "generated"

SKIP_MAC = "skip: SmartSDR-for-Mac UI with no AetherSDR equivalent (ruling 2026-11-13)"
NOLOG = "skip: AetherSDR has no QSO logbook"
NOWIN = "skip: no AetherSDR window for this tool"
B = []  # (num, name, kbd, midi, note)
def b(n, name, k=None, m=None, note=""): B.append((n, name, k, m, note))
b(1,"A <> B","slice_swap_ab","global.sliceSwapAB")
b(2,"A > B","slice_a_to_b","global.sliceAToB")
b(3,"ACC TX toggle","tx_output_acc","tx.outputAcc")
b(4,"ANF","anf_toggle","rx.anfEnable")
b(5,"APF toggle","apf_toggle","rx.apfEnable")
b(6,"ATU","atu_start","tx.atuStart")
b(7,"ATU Bypass","atu_bypass","tx.atuBypass")
b(8,"ATU MEM","atu_memories_toggle","tx.atuMemories")
b(9,"ATU Tune","atu_start","tx.atuStart")
b(10,"B > A","slice_b_to_a","global.sliceBToA")
b(11,"Band Next","band_up","global.bandUp")
b(12,"Band Previous","band_down","global.bandDown")
b(13,"Band zoom toggle","band_zoom","global.bandZoom")
b(14,"Bandwidth 10 KHz","pan_span_10k","global.panSpan10k")
b(15,"Bandwidth 25 KHz","pan_span_25k","global.panSpan25k")
b(16,"Bandwidth 5 KHz","pan_span_5k","global.panSpan5k")
b(17,"Bandwidth 50 KHz","pan_span_50k","global.panSpan50k")
b(18,"Binaural RX toggle","binaural_toggle","rx.binaural")
b(19,"Cancel CWX","cwx_cancel","cwx.cancel")
b(20,"Clear Log Window",note=SKIP_MAC)
b(21,"Close Panadapter","pan_close","global.panClose")
for i,n in zip(range(22,32),[1,10,2,3,4,5,6,7,8,9]): b(i,f"Command {n}",note=SKIP_MAC)
b(32,"Create Panadapter","pan_add","global.panAdd")
b(33,"CWX","cwx_panel_toggle","cwx.panelToggle")
b(34,"DAX toggle","dax_toggle","phone.daxEnable")
b(35,"Delete Slice","slice_close","global.sliceClose")
b(36,"Diversity toggle","diversity_toggle","rx.diversity")
b(37,"Enter Frequency","go_to_freq","global.goToFreq")
b(38,"Filter width 100 Hz","filter_width_100","rx.filterWidth100")
b(39,"Filter width 250 Hz","filter_width_250","rx.filterWidth250")
b(40,"Filter width 3 KHz","filter_width_3000","rx.filterWidth3000")
b(41,"Filter width 400 Hz","filter_width_400","rx.filterWidth400")
b(42,"Frequency Down","tune_down_1","global.tuneDown1")
b(43,"Frequency Down 100x","tune_down_100","global.tuneDown100")
b(44,"Frequency Down 10x","tune_down_10","global.tuneDown10")
b(45,"Frequency Up","tune_up_1","global.tuneUp1")
b(46,"Frequency Up 100x","tune_up_100","global.tuneUp100")
b(47,"Frequency Up 10x","tune_up_10","global.tuneUp10")
for i,n in zip(range(48,53),range(1,6)): b(i,f"Global Profile {n}",f"global_profile_{n}",f"global.globalProfile{n}")
b(53,"Log QSO",note=NOLOG)
for i,n in zip(range(54,66),[1,10,11,12,2,3,4,5,6,7,8,9]): b(i,f"Macro {n}",f"cwx_macro_{n}",f"cwx.macro{n}")
b(66,"Macros Stop","cwx_cancel","cwx.cancel")
b(67,"Main Audio Mute toggle","master_mute_toggle","global.masterMute")
for i,n in zip(range(68,78),[1,10,2,3,4,5,6,7,8,9]): b(i,f"Memory {n}",f"memory_recall_{n}",f"global.memoryRecall{n}")
b(78,"Memory Down","memory_prev","global.memoryPrev")
b(79,"Memory Up","memory_next","global.memoryNext")
for i,(nm,mode) in zip(range(80,91),[("Mode AM","AM"),("Mode CW","CW"),("Mode DIGL","DIGL"),("Mode DIGU","DIGU"),("Mode FM","FM"),("Mode LSB","LSB"),("Mode Next",None),("Mode Previous",None),("Mode RTTY","RTTY"),("Mode SAM","SAM"),("Mode USB","USB")]):
    if mode: b(i,nm,f"mode_{mode.lower()}",f"global.mode{mode}")
    elif nm=="Mode Next": b(i,nm,"mode_up","global.modeUp")
    else: b(i,nm,"mode_down","global.modeDown")
b(91,"MOX","mox_toggle","tx.mox")
b(92,"NB","nb_toggle","rx.nbEnable")
b(93,"Not assigned",note="skip: placeholder")
b(94,"NR","nr_toggle","rx.nrEnable")
b(95,"Open APRS","open_aethermodem","global.openAetherModem","AetherModem: APRS and packet")
b(96,"Open Band Plan Tool","band_plan_toggle","global.bandPlanToggle")
b(97,"Open Call lookup",None,"global.callsignLookup","keyboard: fixed Ctrl+Shift+L")
b(98,"Open CW Decoder","cw_decode_toggle","cw.decodeRx")
b(99,"Open DX-Cluster Tool","open_spothub","global.openSpotHub")
b(100,"Open Frequency List Tool",note=SKIP_MAC)
b(101,"Open FT8 Tool",note=NOWIN)
b(102,"Open Green Heron Tool",note=SKIP_MAC)
b(103,"Open HF-Fax",note=SKIP_MAC)
b(104,"Open Logbook Tool",note=NOLOG)
b(105,"Open Memories","open_memories","global.openMemories")
b(106,"Open Metropwr Tool",note=SKIP_MAC)
b(107,"Open POTA Tool","open_spothub","global.openSpotHub","SpotHub: POTA tab")
b(108,"Open PSK",note=NOWIN)
b(109,"Open PSK Reporter Tool","open_aethermap","global.openAetherMap","AetherMap: PSK Reporter reception map")
b(110,"Open RTTY","rtty_decode_toggle","rtty.decode","the RTTY decoder")
b(111,"Open S.A.T. Controller Tool",note=SKIP_MAC)
b(112,"Open SSTV",note=SKIP_MAC)
b(113,"Open Ultra Beam Tool",note=SKIP_MAC)
b(114,"Open Waveshare Tool",note=SKIP_MAC)
b(115,"PTT Push","ptt_hold","cw.ptt")
b(116,"PTT toggle","mox_toggle","global.txButton")
b(117,"RCA TX 1 toggle","tx_output_1","tx.output1")
b(118,"RCA TX 2 toggle","tx_output_2","tx.output2")
b(119,"RCA TX 3 toggle","tx_output_3","tx.output3")
b(120,"RIT","rit_toggle","rx.ritEnable")
b(121,"RIT Down","rit_offset_down","rx.ritOffset")
b(122,"RIT Up","rit_offset_up","rx.ritOffset")
b(123,"RX Ant 1","rx_antenna_1","rx.antenna1")
b(124,"RX Ant 2","rx_antenna_2","rx.antenna2")
b(125,"RX RX_A","rx_antenna_3","rx.antenna3","by position in the radio's RX antenna list")
b(126,"RX XVTR","rx_antenna_4","rx.antenna4","by position in the radio's RX antenna list")
for i,band in zip(range(127,138),["10m","12m","15m","160m","17m","20m","30m","40m","60m","6m","80m"]): b(i,f"Select {band} Band",f"band_{band}",f"global.band{band}")
for i,n in zip(range(138,146),range(1,9)): b(i,f"Select Panadapter {n}",f"pan_select_{n}",f"global.panSelect{n}")
for i,l in zip(range(146,150),"ABCD"): b(i,f"Select Slice {l}",f"slice_select_{l.lower()}",f"global.sliceSelect{l}")
b(150,"Slice details",note=SKIP_MAC)
b(151,"Slice Lock","lock_toggle","rx.tuneLock")
b(152,"Slice Mute toggle","mute_toggle","rx.mute")
b(153,"Slice new","slice_new","global.sliceNew")
b(154,"Slice next","next_slice","global.nextSlice")
b(155,"Slice set active","next_slice","global.nextSlice","plus Select Slice A–H")
b(156,"Slice set TX","slice_set_tx","global.sliceSetTx")
b(157,"Split toggle","split_toggle","global.splitToggle")
b(158,"Switch Keyer to Iambic-A","cw_keyer_iambic_a","cw.keyerIambicA")
b(159,"Switch Keyer to Iambic-B","cw_keyer_iambic_b","cw.keyerIambicB")
b(160,"Switch Keyer to Straight-Key","cw_keyer_straight","cw.keyerStraight")
b(161,"Trigger CW left Paddle","cwdit","cwdit")
b(162,"Trigger CW right Paddle","cwdah","cwdah")
b(163,"Trigger CW Straight Key","cwkey","cwkey")
b(164,"Tune Stack Down","band_stack_down","global.bandStackDown")
b(165,"Tune Stack Up","band_stack_up","global.bandStackUp")
b(166,"Tune step (1,10,100,1000)","step_up","rx.stepUp")
b(167,"Tune step Previous","step_down","rx.stepDown")
b(168,"TX Ant 1","tx_antenna_1","tx.antenna1")
b(169,"TX Ant 2","tx_antenna_2","tx.antenna2")
b(170,"TX XVTR","tx_antenna_3","tx.antenna3","by position in the radio's TX antenna list")
b(171,"View Command Bar",note=SKIP_MAC)
b(172,"View CWX Panel","cwx_panel_toggle","cwx.panelToggle")
for i,nm in zip(range(173,179),["View DTMF Panel","View Macros Bar","View PTT","View Radio Controls Panel","View Rit/Xit Panel","View Stack Panel"]): b(i,nm,note=SKIP_MAC)
b(179,"VOX","vox_toggle","phone.voxEnable")
b(180,"WNB","wnb_toggle","rx.wnbEnable")
b(181,"XIT","xit_toggle","rx.xitEnable")
b(182,"XIT Down","xit_offset_down","tx.xitOffset")
b(183,"XIT Up","xit_offset_up","tx.xitOffset")
b(184,"Zoom in","pan_zoom_in","global.panZoomIn")
b(185,"Zoom out","pan_zoom_out","global.panZoomOut")
W = []
def w(n,name,k=None,m=None,note=""): W.append((n,name,k,m,note))
w(0,"Mic gain","mic_level_up","phone.micLevel","keyboard: Mic Level Up/Down")
w(1,"Adjust Main Volume","master_volume_up","global.masterVolume","keyboard: Master Volume Up/Down")
w(2,"Slice Frequency (active)","tune_up_1","rx.tuneKnob")
w(3,"Slice Frequency (active)","tune_up_1","rx.tuneKnob")
w(4,"Bandwidth (active)","filter_widen","rx.filterWidth")
w(5,"CW speed","cw_speed_up","cw.speed")
w(6,"AGC Threshold (active)","agct_up","rx.agcThreshold")
w(7,"RF Power","rf_power_up","tx.rfPower")
w(8,"NB-Level (active)","nb_level_up","rx.nbLevel")
w(10,"Slice Volume (active)","af_gain_up","rx.afGain")
w(11,"WNB-Level (active)","wnb_level_up","rx.wnbLevel")
w(12,"APF value (active)","apf_level_up","rx.apfLevel")
w(13,"ANF Level (active)","anf_level_up","rx.anfLevel")
w(14,"NR-Level (active)","nr_level_up","rx.nrLevel")
w(15,"XIT Frequency (active)","xit_offset_up","tx.xitOffset")
w(16,"RIT Frequency (active)","rit_offset_up","rx.ritOffset")


def generated_names(page: str) -> dict:
    text = (GENERATED / page).read_text(encoding="utf-8")
    return {m.group(2): m.group(1).replace("\\", "")
            for m in re.finditer(r"^\| ([^|]+?) \| `([^`]+)` \|", text, re.M)}


def check() -> int:
    kids, mids = generated_names("default-shortcuts.md"), generated_names("midi-actions.md")
    if [r[0] for r in B] != list(range(1, 186)):
        print("error: the button list is not numbered 1..185")
        return 1
    problems = covered = skipped = 0
    for rows in (B, W):
        for n, name, k, m, note in rows:
            if not k and not m:
                if not note.startswith("skip"):
                    print(f"error: {n} {name}: no mapping and no skip reason")
                    problems += 1
                skipped += 1
                continue
            if k and k not in kids:
                print(f"error: {n} {name}: keyboard id {k!r} is not in default-shortcuts.md")
                problems += 1
            if m and m not in mids:
                print(f"error: {n} {name}: MIDI id {m!r} is not in midi-actions.md")
                problems += 1
            covered += 1
    print(f"{len(B) + len(W)} SmartSDR entries: {covered} mapped, {skipped} skipped, "
          f"{problems} problem(s)")
    return 1 if problems else 0


def table() -> str:
    kn, mn = generated_names("default-shortcuts.md"), generated_names("midi-actions.md")
    def esc(text):
        return text.replace("<", "&lt;").replace(">", "&gt;")
    def row(n, name, k, m, note):
        if not k and not m:
            why = ("AetherSDR has no QSO logbook" if "logbook" in note
                   else "Not available: no AetherSDR equivalent")
            return f"| {esc(name)} | — | {why} |"
        key = f"**{kn[k]}**" if k in kn else ("Ctrl+Shift+L (fixed)" if n == 97 else "—")
        mid = f"**{mn[m]}**" if m in mn else "—"
        extra = " (by position in the radio's antenna list)" if "position" in note else ""
        if n == 155:
            extra = " (or Select Slice A–H)"
        return f"| {esc(name)} | {key} | {mid}{extra} |"
    seen, out = set(), ["| SmartSDR for Mac | Keyboard action | MIDI control |", "|---|---|---|"]
    for r in sorted(B, key=lambda r: r[1].lower()):
        if r[1] not in seen:
            seen.add(r[1])
            out.append(row(*r))
    out += ["", "| SmartSDR wheel | Keyboard action | MIDI control |", "|---|---|---|"]
    for r in W:
        if r[1] not in seen:
            seen.add(r[1])
            out.append(row(*r))
    return "\n".join(out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true")
    group.add_argument("--table", action="store_true")
    args = ap.parse_args(argv)
    if args.table:
        print(table())
        return 0
    return check()


if __name__ == "__main__":
    sys.exit(main())
