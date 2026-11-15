package main

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"strconv"
)

func decodePDTestObservation(data []byte) (pdTestObservation, error) {
	var observation pdTestObservation
	bad := errors.New("invalid test observation")
	if len(data) > 512 {
		return observation, bad
	}
	d := json.NewDecoder(bytes.NewReader(data))
	token, err := d.Token()
	if err != nil || token != json.Delim('{') {
		return observation, bad
	}
	seen := map[string]bool{}
	for d.More() {
		token, err = d.Token()
		key, ok := token.(string)
		if err != nil || !ok || seen[key] {
			return observation, bad
		}
		seen[key] = true
		var raw json.RawMessage
		if d.Decode(&raw) != nil || bytes.Equal(raw, []byte("null")) {
			return observation, bad
		}
		switch key {
		case "test_id":
			err = json.Unmarshal(raw, &observation.TestID)
		case "state":
			err = json.Unmarshal(raw, &observation.State)
		case "cleanup_due_ms":
			err = json.Unmarshal(raw, &observation.CleanupDueMS)
		case "remaining_ms":
			err = json.Unmarshal(raw, &observation.RemainingMS)
		default:
			return observation, bad
		}
		if err != nil {
			return observation, bad
		}
	}
	token, err = d.Token()
	if err != nil || token != json.Delim('}') || d.Decode(new(any)) != io.EOF || len(seen) != 4 || !observation.valid() {
		return observation, bad
	}
	return observation, nil
}

const (
	pdTestBegin  byte = 'B'
	pdTestStatus byte = 'G'
	pdTestStop   byte = 'E'
)

func pdTestOpcode(op byte) bool { return op == pdTestBegin || op == pdTestStatus || op == pdTestStop }
func pdTestRequestValid(op byte, id [16]byte, duration uint16) bool {
	if op == pdTestBegin {
		return id == [16]byte{} && duration >= 1 && duration <= 15000
	}
	return (op == pdTestStatus || op == pdTestStop) && id != [16]byte{} && duration == 0
}
func pdTestID(text string) ([16]byte, bool) {
	var id [16]byte
	if !pdCaptureIDValid(text) {
		return id, false
	}
	b, _ := hex.DecodeString(text)
	copy(id[:], b)
	return id, id != [16]byte{}
}

type pdTestObservation struct {
	TestID       string `json:"test_id"`
	State        string `json:"state"`
	CleanupDueMS string `json:"cleanup_due_ms"`
	RemainingMS  int64  `json:"remaining_ms"`
}

func (s pdTestObservation) valid() bool {
	if _, ok := pdTestID(s.TestID); !ok {
		return false
	}
	due, err := strconv.ParseInt(s.CleanupDueMS, 10, 64)
	if err != nil || due <= 0 || strconv.FormatInt(due, 10) != s.CleanupDueMS || s.RemainingMS < 0 || s.RemainingMS > 15000 {
		return false
	}
	return (s.State == "observing" && s.RemainingMS > 0) || ((s.State == "retiring" || s.State == "ended") && s.RemainingMS == 0)
}
