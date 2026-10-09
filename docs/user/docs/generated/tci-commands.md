---
title: "TCI Commands"
description: "The TCI command names AetherSDR's TCI server recognises, generated from the source."
sidebar_position: 8
custom_edit_url: null
generated_by: "tools/docs/gen_reference.py"
generated_from: ["src/core/TciProtocol.cpp", "src/core/TciServer.cpp"]
---

:::info[Generated page]

This page is generated from `src/core/TciProtocol.cpp`, `src/core/TciServer.cpp` by `tools/docs/gen_reference.py`. Do not edit it by hand: change the source, then run `python3 tools/docs/gen_reference.py`.

:::

These are the command names the TCI server recognises from a client. Names are case-insensitive. A command the server does not recognise is ignored, as the TCI specification requires. Recognising a command does not mean every radio supports every value of it; see the TCI Server page for behaviour.

## Audio, IQ and session commands

Handled per client connection: audio and IQ streams, stream format, and sensor reports.

| Command |
|---|
| `audio_start` |
| `audio_stop` |
| `audio_samplerate` |
| `audio_stream_sample_type` |
| `rx_sensors_enable` |
| `tx_sensors_enable` |
| `iq_samplerate` |
| `iq_start` |
| `iq_stop` |
| `spectrum_event` |
| `audio_stream_samples` |
| `tx_stream_audio_buffering` |
| `line_out_start` |
| `line_out_stop` |
| `line_out_recorder` |
| `audio_stream_channels` |

## Radio commands

| Command |
|---|
| `start` |
| `stop` |
| `tx_enable` |
| `cw_msg` |
| `cw_macros` |
| `cw_macros_stop` |
| `spot` |
| `spot_delete` |
| `spot_clear` |
| `keyer` |
| `cw_keyer_speed` |
| `cw_macros_delay` |
| `cw_terminal` |
| `dds` |
| `if` |
| `rx_channel_enable` |
| `rx_volume` |
| `rx_mute` |
| `rx_balance` |
| `mon_enable` |
| `mon_volume` |
| `rx_nb_param` |
| `rx_bin_enable` |
| `rx_anc_enable` |
| `rx_dse_enable` |
| `rx_nf_enable` |
| `digl_offset` |
| `digu_offset` |
| `set_in_focus` |
| `tx_frequency` |
| `vfo_limits` |
| `if_limits` |
| `vfo_lock` |
| `vfo` |
| `modulation` |
| `trx` |
| `tune` |
| `drive` |
| `tune_drive` |
| `mic_level` |
| `rit_enable` |
| `xit_enable` |
| `rit_offset` |
| `xit_offset` |
| `split_enable` |
| `rx_filter_band` |
| `cw_macros_speed` |
| `lock` |
| `sql_enable` |
| `sql_level` |
| `volume` |
| `mute` |
| `agc_mode` |
| `agc_gain` |
| `rx_nb_enable` |
| `rx_nr_enable` |
| `rx_anf_enable` |
| `rx_apf_enable` |

## AetherSDR extensions

Not part of the TCI specification.

| Command |
|---|
| `rx_record` |
| `rx_play` |
| `tx_gain` |
| `active_slice` |
