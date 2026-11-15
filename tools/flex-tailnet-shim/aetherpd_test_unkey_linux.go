//go:build linux

package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Session-scoped serialization includes startup and unobserved termination.
// A canceled/failed request cannot overlap its eventual cleanup with a new run.
type pdTestUnkeys struct {
	mu      sync.Mutex
	closed  bool
	current *pdTestUnkeyRun
	last    *pdTestUnkeyRun
}

type pdTestUnkeyRun struct {
	owner  *pdTestUnkeys
	ctx    context.Context
	cancel context.CancelFunc
	helper *pdBoundedUnkey // Protected by owner.mu, immutable after publication.
	done   chan struct{}   // Child reaped (or no child launched), not RF release.
	id     [16]byte
}

var errPDTestBusy = errors.New("test unkey session busy")

func (s *pdTestUnkeys) start(ctx context.Context, launch func(context.Context) (*pdBoundedUnkey, error)) (*pdTestUnkeyRun, error) {
	s.mu.Lock()
	if s.closed || ctx.Err() != nil {
		s.mu.Unlock()
		return nil, errors.New("test unkey session unavailable or busy")
	}
	if s.current != nil {
		s.mu.Unlock()
		return nil, errPDTestBusy
	}
	var id [16]byte
	if _, err := rand.Read(id[:]); err != nil || id == [16]byte{} {
		s.mu.Unlock()
		return nil, errors.New("test identity unavailable")
	}
	runCtx, cancel := context.WithCancel(ctx)
	run := &pdTestUnkeyRun{owner: s, ctx: runCtx, cancel: cancel, done: make(chan struct{}), id: id}
	s.current = run
	s.last = run
	s.mu.Unlock()
	helper, err := launch(runCtx)
	s.mu.Lock()
	run.helper = helper
	if s.closed || runCtx.Err() != nil {
		err = errors.Join(err, errors.New("test unkey intent retired during startup"))
	}
	if helper == nil && err == nil {
		err = errors.New("test unkey launcher returned no helper")
	}
	if helper != nil && err == nil && !helper.observing() {
		err = errors.New("test unkey helper ended before publication")
	}
	if err != nil {
		cancel()
		if helper != nil {
			helper.close()
		}
	}
	s.mu.Unlock()
	finish := func() {
		cancel()
		s.mu.Lock()
		if s.current == run {
			s.current = nil
		}
		close(run.done)
		s.mu.Unlock()
	}
	if helper == nil {
		finish()
	} else {
		go func() { <-helper.guard.done; finish() }()
	}
	// Always retain this run, including failures whose child is still alive.
	return run, err
}

func (s *pdTestUnkeys) close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.closed = true
	if s.current != nil {
		s.current.cancel()
		if s.current.helper != nil {
			s.current.helper.close()
		}
	}
}

func (s *pdTestUnkeys) wait(ctx context.Context) error {
	s.mu.Lock()
	run := s.current
	s.mu.Unlock()
	if run == nil {
		return nil
	}
	select {
	case <-run.done:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (r *pdTestUnkeyRun) stop() {
	r.owner.mu.Lock()
	defer r.owner.mu.Unlock()
	r.cancel()
	if r.helper != nil {
		r.helper.close()
	}
}

func (r *pdTestUnkeyRun) observing() bool {
	r.owner.mu.Lock()
	defer r.owner.mu.Unlock()
	return !r.owner.closed && r.owner.current == r && r.ctx.Err() == nil && r.helper != nil && r.helper.observing()
}

// Only this trusted session's captured GUI identity is used. No remote caller
// supplies executable paths, socket addresses, client handles or commands.
func (p *aetherPDProcess) startTestUnkey(ctx context.Context, durationMS int) (*pdTestUnkeyRun, error) {
	if p == nil || !p.captureAlive() || p.receiveJournal == nil || p.unkey == nil || durationMS < 1 || durationMS > 15000 {
		return nil, errors.New("test unkey requires an owned prepared session")
	}
	original := p.receiveJournal.original
	if !original.complete || original.guiHandle != p.unkey.owner {
		return nil, errors.New("test unkey original GUI mismatch")
	}
	return p.testUnkeys.start(ctx, func(runCtx context.Context) (*pdBoundedUnkey, error) {
		program, err := os.Executable()
		if err != nil {
			return nil, err
		}
		api, client, err := preparePDTestUnkeyAPI(runCtx, pdStartIdentity{original.clientID, original.guiHandle})
		if err != nil {
			return nil, err
		}
		defer api.Close()
		return startPDBoundedUnkey(runCtx, filepath.Join(filepath.Dir(program), "pd-unkey-guard"), api, client, original.guiHandle, durationMS)
	})
}

func preparePDTestUnkeyAPI(ctx context.Context, id pdStartIdentity) (*os.File, uint32, error) {
	if _, err := id.frame(); err != nil {
		return nil, 0, err
	}
	ctx, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()
	conn, err := (&net.Dialer{}).DialContext(ctx, "tcp4", "127.0.0.1:4992")
	if err != nil {
		return nil, 0, err
	}
	defer conn.Close()
	stopCancel := context.AfterFunc(ctx, func() { conn.Close() })
	defer stopCancel()
	deadline, _ := ctx.Deadline()
	if err = conn.SetDeadline(deadline); err != nil {
		return nil, 0, err
	}
	client, err := bindPDTestUnkeyAPI(ctx, conn, id)
	if err != nil {
		return nil, 0, err
	}
	if err = conn.SetDeadline(time.Time{}); err != nil {
		return nil, 0, err
	}
	file, err := conn.(*net.TCPConn).File()
	if err != nil {
		return nil, 0, err
	}
	if err = ctx.Err(); err != nil {
		file.Close()
		return nil, 0, err
	}
	return file, client, nil
}

// Mirrors the native session provider's observed firmware 4.3.16 bind reply:
// validate the returned GUI handle, then leave a second ACK for the guardian.
// Single-byte reads deliberately leave all following status bytes in TCP.
func bindPDTestUnkeyAPI(ctx context.Context, conn io.ReadWriter, id pdStartIdentity) (uint32, error) {
	if _, err := id.frame(); err != nil {
		return 0, err
	}
	if err := ctx.Err(); err != nil {
		return 0, err
	}
	if err := pdWriteAll(conn, []byte("C970101|client bind client_id="+id.GUIClientID+"\n")); err != nil {
		return 0, err
	}
	var client uint32
	bound := false
	line := make([]byte, 0, 256)
	var one [1]byte
	for total := 0; total < 65536; total++ {
		if err := ctx.Err(); err != nil {
			return 0, err
		}
		if _, err := io.ReadFull(conn, one[:]); err != nil {
			return 0, err
		}
		if one[0] != '\n' {
			if one[0] < 32 && one[0] != '\r' || one[0] > 126 || len(line) >= 4096 {
				return 0, errors.New("invalid test bind frame")
			}
			line = append(line, one[0])
			continue
		}
		text := strings.TrimSuffix(string(line), "\r")
		line = line[:0]
		if text == "" || strings.ContainsRune(text, '\r') {
			return 0, errors.New("invalid test bind line")
		}
		switch text[0] {
		case 'H':
			handle, err := pdTestHex(text[1:])
			if err != nil || client != 0 || handle == 0 || handle == id.GUIHandle {
				return 0, errors.New("invalid test bind client handle")
			}
			client = handle
		case 'R':
			parts := strings.Split(text[1:], "|")
			if bound || len(parts) != 3 || parts[0] != "970101" || !strings.HasPrefix(parts[2], "0x") {
				return 0, errors.New("invalid test bind reply")
			}
			code, codeErr := pdTestHex(parts[1])
			gui, guiErr := pdTestHex(strings.TrimPrefix(parts[2], "0x"))
			if codeErr != nil || guiErr != nil || code != 0 || gui != id.GUIHandle {
				return 0, errors.New("test bind did not confirm original GUI")
			}
			bound = true
		case 'V', 'S', 'M':
		default:
			return 0, errors.New("unknown test bind frame")
		}
		if client != 0 && bound {
			if err := ctx.Err(); err != nil {
				return 0, err
			}
			if err := pdWriteAll(conn, []byte("C910000|client bind client_id="+id.GUIClientID+"\n")); err != nil {
				return 0, err
			}
			return client, nil
		}
	}
	return 0, fmt.Errorf("test bind exceeds %d bytes", 65536)
}

func pdTestHex(text string) (uint32, error) {
	if len(text) == 0 || len(text) > 8 {
		return 0, errors.New("invalid test bind hex length")
	}
	for _, c := range text {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f' || c >= 'A' && c <= 'F') {
			return 0, errors.New("invalid test bind hex digit")
		}
	}
	n, err := strconv.ParseUint(text, 16, 32)
	return uint32(n), err
}

func (p *aetherPDProcess) testUnkeyOperation(ctx context.Context, op byte, id [16]byte, duration uint16) pdSessionReply {
	if p == nil {
		return pdCaptureJSON(503, map[string]string{"state": "test_unavailable"})
	}
	return p.testUnkeys.operation(ctx, op, id, duration, p.startTestUnkey)
}

func (s *pdTestUnkeys) operation(ctx context.Context, op byte, id [16]byte, duration uint16,
	start func(context.Context, int) (*pdTestUnkeyRun, error)) pdSessionReply {
	unavailable := func() pdSessionReply { return pdCaptureJSON(503, map[string]string{"state": "test_unavailable"}) }
	if ctx.Err() != nil || !pdTestRequestValid(op, id, duration) {
		return unavailable()
	}
	if op == pdTestBegin {
		// A successful reply ends this request, not the run. Session Stop owns
		// cancellation afterwards; the native cleanup deadline is never renewed.
		runCtx, cancel := context.WithCancel(context.Background())
		stopBridge := context.AfterFunc(ctx, cancel)
		run, err := start(runCtx, int(duration))
		detached := stopBridge()
		if err != nil || !detached || ctx.Err() != nil || run == nil {
			cancel()
			if run != nil {
				run.stop()
			}
			if errors.Is(err, errPDTestBusy) {
				return pdCaptureJSON(409, map[string]string{"state": "test_busy"})
			}
			return unavailable()
		}
		id = run.id
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return unavailable()
	}
	run := s.last
	if run == nil || run.id != id {
		return pdCaptureJSON(404, map[string]string{"state": "test_not_found"})
	}
	if ctx.Err() != nil {
		if op != pdTestStatus {
			run.cancel()
			if run.helper != nil {
				run.helper.close()
			}
		}
		return unavailable()
	}
	if op == pdTestStop {
		run.cancel()
		if run.helper != nil {
			run.helper.close()
		}
	}
	if run.helper == nil {
		return unavailable()
	}
	observation := pdTestObservation{TestID: hex.EncodeToString(id[:]), State: "retiring", CleanupDueMS: strconv.FormatInt(run.helper.guard.cleanupDue, 10)}
	if s.current == run && run.ctx.Err() == nil && run.helper.observing() {
		now, err := pdMonotonicMS()
		remaining := run.helper.guard.cleanupDue - now
		if err == nil && now > 0 && remaining > 0 && remaining <= 15000 {
			observation.State = "observing"
			observation.RemainingMS = remaining
		} else {
			run.cancel()
			run.helper.close()
		}
	}
	select {
	case <-run.done:
		observation.State = "ended"
		observation.RemainingMS = 0
	default:
	}
	if op == pdTestBegin && observation.State != "observing" {
		run.cancel()
		run.helper.close()
		return unavailable()
	}
	code := 200
	if op == pdTestStop {
		code = 202
	}
	return pdCaptureJSON(code, observation)
}
