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

type pdSessionStart struct{ cancel context.CancelFunc }

func decodePDStart(r io.Reader) (pdStartIdentity, error) {
	var id pdStartIdentity
	data, err := io.ReadAll(io.LimitReader(r, 513))
	if err != nil || len(data) > 512 {
		return id, errors.New("invalid start request size")
	}
	dec := json.NewDecoder(bytes.NewReader(data))
	token, err := dec.Token()
	if err != nil || token != json.Delim('{') {
		return id, errors.New("expected start object")
	}
	seen := map[string]bool{}
	for dec.More() {
		token, err = dec.Token()
		if err != nil {
			return id, err
		}
		key, ok := token.(string)
		if !ok || seen[key] {
			return id, errors.New("duplicate start field")
		}
		seen[key] = true
		var raw json.RawMessage
		if err = dec.Decode(&raw); err != nil {
			return id, err
		}
		if bytes.Equal(raw, []byte("null")) {
			return id, errors.New("null start field")
		}
		switch key {
		case "gui_client_id":
			err = json.Unmarshal(raw, &id.GUIClientID)
		case "gui_handle":
			err = json.Unmarshal(raw, &id.GUIHandle)
		default:
			return id, errors.New("unknown start field")
		}
		if err != nil {
			return id, err
		}
	}
	if _, err = dec.Token(); err != nil {
		return id, err
	}
	if _, err = dec.Token(); err != io.EOF {
		return id, errors.New("trailing start data")
	}
	if len(seen) != 2 {
		return id, errors.New("missing start identity")
	}
	_, err = id.frame()
	return id, err
}

// Caller holds n.mu. Revocation closes pending and established sessions.
func (n *Node) retirePDSessionLocked() {
	if n.pdInstall != nil {
		n.pdInstall.cancel()
	}
	if n.pdStarting != nil {
		n.pdStarting.cancel()
	}
	if n.pdSession != nil {
		n.pdSession.Close()
	}
}
func aetherPDStartHandler(n *Node) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		token := bearer(r)
		if !n.checkToken(token) {
			writeJSON(w, 401, apiError{errUnauthorized.Error()})
			return
		}
		if r.URL.RawQuery != "" {
			writeJSON(w, 400, apiError{"start accepts only a GUI identity body"})
			return
		}
		id, err := decodePDStart(r.Body)
		if err != nil {
			writeJSON(w, 400, apiError{err.Error()})
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 20*time.Second)
		defer cancel()
		pending := &pdSessionStart{cancel: cancel}
		n.mu.Lock()
		if !n.checkTokenLocked(token) {
			n.mu.Unlock()
			writeJSON(w, 401, apiError{errUnauthorized.Error()})
			return
		}
		if n.pdInstallActiveLocked() || n.pdProcess != nil || n.pdStarting != nil || (n.pdSession != nil && n.pdSession.alive() && (!n.pdSessionStopped || n.pdSession.busy())) {
			n.mu.Unlock()
			writeJSON(w, 409, apiError{"AetherPD session already owned or starting"})
			return
		}
		if n.pdSession != nil {
			n.pdSession.Close()
			n.pdSession = nil
		}
		n.pdStarting = pending
		dial := n.pdSessionDial
		if dial == nil {
			dial = dialPDSession
		}
		n.mu.Unlock()
		channel, err := dial(ctx, id)
		n.mu.Lock()
		authorized := n.checkTokenLocked(token)
		valid := authorized && n.pdStarting == pending && ctx.Err() == nil && err == nil && channel != nil
		if n.pdStarting == pending {
			n.pdStarting = nil
		}
		if valid {
			n.pdSession = channel
			n.pdSessionStopped = false
		} else if channel != nil {
			channel.Close()
		}
		n.mu.Unlock()
		if !authorized {
			writeJSON(w, 401, apiError{errUnauthorized.Error()})
			return
		}
		if !valid {
			writeJSON(w, 503, map[string]string{"state": "start_unavailable"})
			return
		}
		// Receipt of a private channel is not observed DSP registration/correction.
		cancelOnDisconnect := context.AfterFunc(r.Context(), channel.Close)
		defer cancelOnDisconnect()
		writeJSON(w, 202, map[string]string{"state": "start_requested"})
	}
}

// A terminal supervisor observation permits an explicit successor request.
// It does not confirm cleanup, close the channel or start another session.
func pdSessionTerminalStatus(reply pdSessionReply) bool {
	if reply.err != nil || reply.code != http.StatusOK || len(reply.body) > pdSessionMaxBody {
		return false
	}
	dec := json.NewDecoder(bytes.NewReader(reply.body))
	if token, err := dec.Token(); err != nil || token != json.Delim('{') {
		return false
	}
	seen := map[string]bool{}
	var state string
	for dec.More() {
		token, err := dec.Token()
		key, ok := token.(string)
		if err != nil || !ok || seen[key] {
			return false
		}
		seen[key] = true
		switch key {
		case "available":
			value, err := dec.Token()
			if err != nil || value != false {
				return false
			}
		case "state":
			if err := dec.Decode(&state); err != nil {
				return false
			}
		default:
			return false
		}
	}
	if token, err := dec.Token(); err != nil || token != json.Delim('}') || len(seen) != 2 {
		return false
	}
	if _, err := dec.Token(); err != io.EOF {
		return false
	}
	return state == "stopped" || state == "cleanup_unconfirmed" || state == "stopped_receive_unconfirmed"
}

func aetherPDSessionHandler(n *Node, opcode byte, local http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		token := bearer(r)
		n.mu.Lock()
		authorized := n.checkTokenLocked(token)
		channel := n.pdSession
		pending := n.pdStarting
		if authorized && opcode == 'T' && pending != nil && r.URL.RawQuery == "" && r.ContentLength == 0 {
			pending.cancel()
		}
		n.mu.Unlock()
		if !authorized {
			writeJSON(w, 401, apiError{errUnauthorized.Error()})
			return
		}
		if channel == nil && pending == nil {
			local(w, r)
			return
		}
		if r.URL.RawQuery != "" || r.ContentLength != 0 {
			writeJSON(w, 400, apiError{"this endpoint accepts no parameters or body"})
			return
		}
		if pending != nil {
			if opcode == 'T' {
				writeJSON(w, 202, map[string]string{"state": "stop_requested"})
			} else {
				writeJSON(w, 503, map[string]any{"available": false, "state": "starting"})
			}
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 8*time.Second)
		defer cancel()
		reply := channel.Call(ctx, opcode)
		ended := opcode == 'S' && pdSessionTerminalStatus(reply)
		n.mu.Lock()
		authorized = n.checkTokenLocked(token)
		current := n.pdSession == channel
		if authorized && current && ((opcode == 'T' && reply.err == nil && reply.code == 202) || ended) {
			n.pdSessionStopped = true
		}
		n.mu.Unlock()
		if !authorized {
			channel.Close()
			writeJSON(w, 401, apiError{errUnauthorized.Error()})
			return
		}
		if reply.err != nil || !current {
			writeJSON(w, 503, map[string]any{"available": false, "state": "unavailable"})
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(reply.code)
		_, _ = w.Write(reply.body)
	}
}
