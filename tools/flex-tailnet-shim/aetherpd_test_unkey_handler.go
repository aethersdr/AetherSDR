package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"time"
)

func decodePDTestDuration(r io.Reader) (uint16, error) {
	data, err := io.ReadAll(io.LimitReader(r, 129))
	if err != nil || len(data) > 128 {
		return 0, errors.New("invalid test duration body")
	}
	d := json.NewDecoder(bytes.NewReader(data))
	opening, err := d.Token()
	if err != nil || opening != json.Delim('{') {
		return 0, errors.New("expected test object")
	}
	key, err := d.Token()
	if err != nil || key != "cleanup_after_ms" {
		return 0, errors.New("expected cleanup_after_ms")
	}
	var duration uint16
	if err = d.Decode(&duration); err != nil || duration < 1 || duration > 15000 {
		return 0, errors.New("cleanup_after_ms must be 1..15000")
	}
	closing, err := d.Token()
	if err != nil || closing != json.Delim('}') || d.Decode(new(any)) != io.EOF {
		return 0, errors.New("extra test fields or data")
	}
	return duration, nil
}

func aetherPDTestUnkeyHandler(n *Node, op byte) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		token := bearer(r)
		n.mu.Lock()
		authorized := n.checkTokenLocked(token)
		channel, process := n.pdSession, n.pdProcess
		usable := n.pdStarting == nil && !n.pdSessionStopped && (channel != nil || process != nil)
		n.mu.Unlock()
		if !authorized {
			writeJSON(w, 401, apiError{errUnauthorized.Error()})
			return
		}
		if r.URL.RawQuery != "" {
			writeJSON(w, 400, apiError{"test operations accept no query"})
			return
		}
		var id [16]byte
		var duration uint16
		var err error
		if op == pdTestBegin {
			duration, err = decodePDTestDuration(r.Body)
		} else {
			var valid bool
			id, valid = pdTestID(r.PathValue("testID"))
			if !valid || r.ContentLength != 0 {
				err = errors.New("invalid test ID or unexpected body")
			}
		}
		if err != nil {
			writeJSON(w, 400, apiError{err.Error()})
			return
		}
		unavailable := func() { writeJSON(w, 503, map[string]string{"state": "test_unavailable"}) }
		if !usable || (channel != nil && !channel.testSupported) {
			unavailable()
			return
		}
		retire := func() {
			if channel != nil {
				channel.Close()
			} else if process != nil {
				process.Stop()
			}
		}
		stopCancel := func() bool { return true }
		if op == pdTestBegin {
			stopCancel = context.AfterFunc(r.Context(), retire)
		}
		defer stopCancel()
		ctx, cancel := context.WithTimeout(r.Context(), 7*time.Second)
		defer cancel()
		var reply pdSessionReply
		if channel != nil {
			reply = channel.CallTest(ctx, op, id, duration)
		} else {
			reply = process.testUnkeyOperation(ctx, op, id, duration)
		}
		n.mu.Lock()
		authorized = n.checkTokenLocked(token)
		current := n.pdSession == channel && n.pdProcess == process && n.pdStarting == nil && !n.pdSessionStopped
		n.mu.Unlock()
		if !authorized {
			retire()
			writeJSON(w, 401, apiError{errUnauthorized.Error()})
			return
		}
		if !current || ctx.Err() != nil || reply.err != nil || (channel != nil && !channel.alive()) {
			if op == pdTestBegin {
				retire()
			}
			unavailable()
			return
		}
		if reply.code != 200 && reply.code != 202 {
			switch reply.code {
			case 409:
				writeJSON(w, 409, map[string]string{"state": "test_busy"})
			case 404:
				writeJSON(w, 404, map[string]string{"state": "test_not_found"})
			default:
				unavailable()
			}
			return
		}
		observation, decodeErr := decodePDTestObservation(reply.body)
		if decodeErr != nil ||
			(op != pdTestBegin && observation.TestID != r.PathValue("testID")) ||
			(op == pdTestBegin && (reply.code != 200 || observation.State != "observing" || observation.RemainingMS > int64(duration))) ||
			(op == pdTestStatus && reply.code != 200) || (op == pdTestStop && (reply.code != 202 || observation.State == "observing")) {
			if op == pdTestBegin {
				retire()
			}
			unavailable()
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(reply.code)
		if err := json.NewEncoder(w).Encode(observation); err != nil && op == pdTestBegin {
			retire()
		}
	}
}
