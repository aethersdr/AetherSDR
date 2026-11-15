package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"strconv"
)

// Source identity is scoped to the original owned DSP. Tones describes the
// guarded emitted-source interval, not calibrated power or ADC headroom.
type pdMeasurementSource struct {
	SourceID string `json:"source_id"`
	Tones    bool   `json:"tones"`
}

func parsePDMeasurementSource(raw json.RawMessage) (*pdMeasurementSource, error) {
	bad := errors.New("invalid measurement provenance")
	dec := json.NewDecoder(bytes.NewReader(raw))
	token, err := dec.Token()
	if err != nil || token != json.Delim('{') {
		return nil, bad
	}
	var result pdMeasurementSource
	seen := map[string]bool{}
	for dec.More() {
		token, err = dec.Token()
		key, ok := token.(string)
		if err != nil || !ok || seen[key] {
			return nil, bad
		}
		seen[key] = true
		switch key {
		case "source_id":
			token, err = dec.Token()
			result.SourceID, ok = token.(string)
		case "tones":
			token, err = dec.Token()
			result.Tones, ok = token.(bool)
		default:
			return nil, bad
		}
		if err != nil || !ok {
			return nil, bad
		}
	}
	if token, err = dec.Token(); err != nil || token != json.Delim('}') || len(seen) != 2 {
		return nil, bad
	}
	if _, err = dec.Token(); err != io.EOF {
		return nil, bad
	}
	if len(result.SourceID) != 16 {
		return nil, bad
	}
	for _, c := range result.SourceID {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return nil, bad
		}
	}
	id, err := strconv.ParseUint(result.SourceID, 16, 64)
	if err != nil || (result.Tones && id == 0) {
		return nil, bad
	}
	return &result, nil
}
