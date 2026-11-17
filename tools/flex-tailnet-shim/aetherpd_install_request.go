package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"io"
	"net/http"
	"time"

	"golang.org/x/crypto/blake2b"
)

// Only the public receipt and a domain-separated credential commitment belong
// in an upload artifact. The private configuration lives for one bounded job.
func pdInstallPublic(config []byte) ([120]byte, error) {
	var public [120]byte
	if len(config) != 120 || string(config[:8]) != "PDXFER01" || string(config[40:48]) != "PDDELIV1" || bytes.Equal(config[8:40], make([]byte, 32)) {
		return public, errors.New("invalid installer request configuration")
	}
	arch, role := binary.BigEndian.Uint32(config[48:52]), binary.BigEndian.Uint32(config[52:56])
	if (arch != 7 && arch != 64) || role != 2 {
		return public, errors.New("only an installed-radio upgrade is supported")
	}
	copy(public[:8], "PDPUB001")
	copy(public[8:88], config[40:])
	hash, _ := blake2b.New256(nil)
	hash.Write([]byte("PDKEY001"))
	hash.Write(config)
	copy(public[88:], hash.Sum(nil))
	return public, nil
}

type pdInstallRun func(context.Context, [120]byte, func()) (int, error)
type pdInstallTask struct {
	cancel context.CancelFunc
	done   chan struct{}
	public [120]byte
	result int
	failed bool
}

func (t *pdInstallTask) id() string { return hex.EncodeToString(t.public[88:]) }

// Caller holds n.mu; completion closes done while holding that same lock.
func (n *Node) pdInstallActiveLocked() bool {
	if n.pdInstall == nil {
		return false
	}
	select {
	case <-n.pdInstall.done:
		return false
	default:
		return true
	}
}
func pdInstallBody(r *http.Request) ([120]byte, error) {
	var config [120]byte
	if r.URL.RawQuery != "" || r.Header.Get("Content-Type") != "application/octet-stream" || r.ContentLength != 120 {
		return config, errors.New("expected one fixed installer configuration")
	}
	data, err := io.ReadAll(io.LimitReader(r.Body, 121))
	defer clear(data)
	if err != nil || len(data) != 120 {
		return config, errors.New("invalid installer configuration size")
	}
	if _, err = pdInstallPublic(data); err != nil {
		return config, err
	}
	copy(config[:], data)
	return config, nil
}
func aetherPDInstallRequestHandler(n *Node) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		token := bearer(r)
		if !n.checkToken(token) {
			writeJSON(w, 401, apiError{errUnauthorized.Error()})
			return
		}
		config, err := pdInstallBody(r)
		defer clear(config[:])
		if err != nil {
			writeJSON(w, 400, apiError{err.Error()})
			return
		}
		public, _ := pdInstallPublic(config[:])
		ctx, cancel := context.WithTimeout(context.Background(), 400*time.Second)
		task := &pdInstallTask{cancel: cancel, done: make(chan struct{}), public: public, result: 6}
		n.mu.Lock()
		if !n.checkTokenLocked(token) {
			n.mu.Unlock()
			cancel()
			writeJSON(w, 401, apiError{errUnauthorized.Error()})
			return
		}
		if n.pdInstallActiveLocked() || n.pdProcess != nil || n.pdStarting != nil || (n.pdSession != nil && n.pdSession.alive() && (!n.pdSessionStopped || !n.pdSessionCleanupConfirmed || n.pdSession.busy())) {
			n.mu.Unlock()
			cancel()
			writeJSON(w, 409, apiError{"AetherPD operation already active"})
			return
		}
		if n.pdInstall != nil && n.pdInstall.public == public {
			n.mu.Unlock()
			cancel()
			writeJSON(w, 409, apiError{"installer request already used"})
			return
		}
		if n.pdSession != nil {
			n.pdSession.Close()
			n.pdSession = nil
			n.pdSessionCleanupConfirmed = false
		}
		n.pdInstall = task
		run := n.pdInstallRun
		if run == nil {
			run = runPDInstallRequest
		}
		n.mu.Unlock()
		ready := make(chan struct{}, 1)
		go func(private [120]byte) {
			defer clear(private[:])
			defer cancel()
			result, runErr := run(ctx, private, func() {
				select {
				case ready <- struct{}{}:
				default:
				}
			})
			n.mu.Lock()
			task.result = result
			task.failed = runErr != nil
			close(task.done)
			n.mu.Unlock()
		}(config)
		timer := time.NewTimer(5 * time.Second)
		defer timer.Stop()
		select {
		case <-r.Context().Done():
			cancel()
			return
		case <-timer.C:
			cancel()
			writeJSON(w, 503, apiError{"installer did not become ready"})
			return
		case <-task.done:
			writeJSON(w, 503, apiError{"installer startup failed"})
			return
		case <-ready:
		}
		n.mu.Lock()
		authorized := n.checkTokenLocked(token)
		current := n.pdInstall == task && n.pdInstallActiveLocked()
		n.mu.Unlock()
		if !authorized {
			cancel()
			writeJSON(w, 401, apiError{errUnauthorized.Error()})
			return
		}
		if !current {
			cancel()
			writeJSON(w, 503, apiError{"installer no longer active"})
			return
		}
		writeJSON(w, 202, map[string]any{"request_id": task.id(), "state": "awaiting_inputs", "port": 48995, "installation_committed": false})
	}
}
func aetherPDInstallStateHandler(n *Node, stop bool) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		n.mu.Lock()
		if !n.checkTokenLocked(bearer(r)) {
			n.mu.Unlock()
			writeJSON(w, 401, apiError{errUnauthorized.Error()})
			return
		}
		if r.URL.RawQuery != "" || r.ContentLength != 0 {
			n.mu.Unlock()
			writeJSON(w, 400, apiError{"installer state accepts no parameters or body"})
			return
		}
		task := n.pdInstall
		if task == nil || r.PathValue("requestID") != task.id() {
			n.mu.Unlock()
			writeJSON(w, 404, apiError{"unknown installer request"})
			return
		}
		active := n.pdInstallActiveLocked()
		if stop && active {
			task.cancel()
			n.mu.Unlock()
			writeJSON(w, 202, map[string]string{"state": "stop_requested"})
			return
		}
		state := "running"
		if !active {
			state = "delivery_finished"
			if task.failed {
				state = "unavailable"
			}
		}
		reply := map[string]any{"request_id": task.id(), "state": state, "installation_committed": false}
		if !active {
			reply["delivery_result"] = task.result
		}
		n.mu.Unlock()
		writeJSON(w, 200, reply)
	}
}
