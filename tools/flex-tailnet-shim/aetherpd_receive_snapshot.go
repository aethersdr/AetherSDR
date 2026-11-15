package main

import (
	"errors"
	"strconv"
	"strings"
)

func pdReceiveHexHandle(text string) (uint32, error) {
	if len(text) != 10 || text[:2] != "0x" {
		return 0, errors.New("invalid radio handle")
	}
	for _, c := range []byte(text[2:]) {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f' || c >= 'A' && c <= 'F') {
			return 0, errors.New("invalid radio handle")
		}
	}
	n, err := strconv.ParseUint(text[2:], 16, 32)
	if err != nil || n == 0 {
		return 0, errors.New("invalid radio handle")
	}
	return uint32(n), nil
}

func pdReceiveDecimal(text string) (int, error) {
	if text == "" {
		return 0, errors.New("missing decimal field")
	}
	for i, c := range []byte(text) {
		if c == '-' && i == 0 && len(text) > 1 {
			continue
		}
		if c < '0' || c > '9' {
			return 0, errors.New("invalid decimal field")
		}
	}
	return strconv.Atoi(text)
}

func pdReceiveFrequencyHz(text string) (int64, error) {
	parts := strings.Split(text, ".")
	if len(parts) > 2 || parts[0] == "" {
		return 0, errors.New("invalid MHz field")
	}
	for _, p := range parts {
		if p == "" {
			return 0, errors.New("invalid MHz field")
		}
		for _, c := range []byte(p) {
			if c < '0' || c > '9' {
				return 0, errors.New("invalid MHz field")
			}
		}
	}
	whole, err := strconv.ParseInt(parts[0], 10, 64)
	if err != nil || whole > 1_000_000 {
		return 0, errors.New("MHz field out of range")
	}
	fraction := int64(0)
	if len(parts) == 2 {
		if len(parts[1]) > 6 {
			return 0, errors.New("sub-Hz precision unsupported")
		}
		value := parts[1] + strings.Repeat("0", 6-len(parts[1]))
		fraction, err = strconv.ParseInt(value, 10, 64)
		if err != nil {
			return 0, err
		}
	}
	hz := whole*1_000_000 + fraction
	if hz <= 0 || hz > 1_000_000_000_000 {
		return 0, errors.New("frequency out of range")
	}
	return hz, nil
}

// Capture only a complete radio status; delta merging and subscription barriers
// belong to the observer. No absent field may silently become a default.
func capturePDReceiveOriginal(line string, guiHandle uint32, clientID string, expectedPan uint32, panGain int) (pdReceiveOriginal, error) {
	var out pdReceiveOriginal
	if len(line) > 4096 || len(line) < 5 || line[0] != 'S' || strings.Count(line, "|") != 1 {
		return out, errors.New("invalid slice status")
	}
	for _, c := range []byte(line) {
		if c < 32 || c > 126 {
			return out, errors.New("non-ASCII or multiline status")
		}
	}
	divider := strings.IndexByte(line, '|')
	if divider < 2 || divider > 9 {
		return out, errors.New("invalid status header")
	}
	if _, err := strconv.ParseUint(line[1:divider], 16, 32); err != nil {
		return out, errors.New("invalid status header")
	}
	words := strings.Fields(line[divider+1:])
	if len(words) < 3 || words[0] != "slice" {
		return out, errors.New("expected slice status")
	}
	id, err := pdReceiveDecimal(words[1])
	if err != nil || id < 0 || id >= 16 {
		return out, errors.New("invalid slice ID")
	}
	fields := make(map[string]string, len(words)-2)
	for _, word := range words[2:] {
		key, value, ok := strings.Cut(word, "=")
		if !ok || key == "" {
			return out, errors.New("invalid slice attribute")
		}
		if _, duplicate := fields[key]; duplicate {
			return out, errors.New("duplicate slice attribute")
		}
		fields[key] = value
	}
	owner, err := pdReceiveHexHandle(fields["client_handle"])
	if err != nil || owner != guiHandle {
		return out, errors.New("slice is not owned by prepared GUI")
	}
	pan, err := pdReceiveHexHandle(fields["pan"])
	if err != nil || pan != expectedPan {
		return out, errors.New("slice pan mismatch")
	}
	if fields["in_use"] != "1" || fields["sample_rate"] != "24000" || !pdReceiveClientIDValid(clientID) || panGain < -100 || panGain > 100 {
		return out, errors.New("unsupported or incomplete receive snapshot")
	}
	settings := pdReceiveSettings{mode: fields["mode"], antenna: fields["rxant"], agcMode: fields["agc_mode"]}
	settings.frequencyHz, err = pdReceiveFrequencyHz(fields["RF_frequency"])
	if err != nil {
		return out, err
	}
	for _, f := range []struct {
		key string
		dst *int
	}{
		{"filter_lo", &settings.filterLow}, {"filter_hi", &settings.filterHigh}, {"agc_threshold", &settings.agcThreshold}, {"agc_off_level", &settings.agcOffLevel},
		{"audio_level", &settings.audioLevel}, {"audio_pan", &settings.audioPan}, {"dax", &settings.daxChannel}, {"rit_freq", &settings.ritHz},
		{"squelch_level", &settings.squelchLevel}, {"nr_level", &settings.nrLevel}, {"nb_level", &settings.nbLevel}, {"apf_level", &settings.apfLevel},
	} {
		value, err := pdReceiveDecimal(fields[f.key])
		if err != nil {
			return out, errors.New("missing or invalid receive field: " + f.key)
		}
		*f.dst = value
	}
	for _, f := range []struct {
		key string
		dst *bool
	}{{"audio_mute", &settings.audioMute}, {"rit_on", &settings.ritOn}, {"squelch", &settings.squelch}, {"nr", &settings.nr}, {"nb", &settings.nb}, {"anf", &settings.anf}, {"apf", &settings.apf}} {
		value, ok := fields[f.key]
		if !ok || value != "0" && value != "1" {
			return out, errors.New("missing or invalid receive switch: " + f.key)
		}
		*f.dst = value == "1"
	}
	if !pdReceiveSettingsValid(settings) {
		return out, errors.New("invalid receive settings")
	}
	step, recreateSafe := pdReceiveRecreateProfile(fields, settings.mode)
	return pdReceiveOriginal{complete: true, recreateSafe: recreateSafe, recreateStep: step, guiHandle: guiHandle, clientID: clientID, sliceID: id, panID: pan, panGain: panGain, settings: settings}, nil
}
