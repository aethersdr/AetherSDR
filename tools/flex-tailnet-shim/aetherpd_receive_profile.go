package main

// Allocation restores only the measured basic SSB profile. A profile we cannot
// reproduce must retain manual recovery rather than silently lose settings.
func pdReceiveRecreateProfile(fields map[string]string, mode string) (int, bool) {
	if mode != "USB" && mode != "LSB" || fields["chain_mode"] != mode {
		return 0, false
	}
	step, err := pdReceiveDecimal(fields["step"])
	if err != nil || step < 1 || step > 1_000_000 {
		return 0, false
	}
	for key, expected := range map[string]string{
		"lock": "0", "xit_on": "0", "xit_freq": "0", "tx_offset_freq": "0.000000",
		"txant": "ANT1", "loopa": "0", "loopb": "0", "qsk": "0", "wide": "0",
		"agc_threshold_mode": "manual", "record": "0", "play": "disabled",
		"nr": "0", "nr_wlen": "256", "nr_delay": "25", "nr_adapt_mode": "auto", "nr_isdft_mode": "half",
		"nrl": "0", "nrl_level": "100", "nrl_filter_size": "128", "nrl_delay": "16", "nrl_leakage_level": "50",
		"nrf": "0", "nrf_winc": "256", "nrf_wlen": "2048", "rnn": "0", "nrs": "0", "nrs_level": "50",
		"anf": "0", "anf_level": "50", "anf_wlen": "256", "anf_delay": "25", "anf_adapt_mode": "auto", "anf_isdft_mode": "half",
		"anfl": "0", "anfl_level": "100", "anfl_filter_size": "128", "anfl_delay": "16", "anfl_leakage_level": "0",
		"anft": "0", "anft_min_db": "-110", "anft_tone_db_diff": "12", "nb": "0", "apf": "0", "wnb": "0", "wnb_level": "90",
		"diversity": "0", "diversity_parent": "0", "diversity_child": "0", "esc": "0", "esc_gain": "1.000000", "esc_phase_shift": "0.000000",
		"fm_tone_mode": "OFF", "fm_tone_value": "67.0", "fm_repeater_offset_freq": "0.000000", "repeater_offset_dir": "SIMPLEX",
		"fm_tone_burst": "0", "fm_deviation": "5000", "dfm_pre_de_emphasis": "0", "post_demod_low": "300", "post_demod_high": "3300",
		"rtty_mark": "2125", "rtty_shift": "170", "digl_offset": "1500", "digu_offset": "1500", "post_demod_bypass": "0",
	} {
		if fields[key] != expected {
			return 0, false
		}
	}
	return step, true
}
