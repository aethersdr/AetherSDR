//go:build linux

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"os"
	"strconv"
	"sync"
	"sync/atomic"

	"golang.org/x/sys/unix"
)

func pdMonotonicMS() (int64, error) {
	var now unix.Timespec
	if err := unix.ClockGettime(unix.CLOCK_MONOTONIC, &now); err != nil {
		return 0, err
	}
	return now.Nano() / 1_000_000, nil
}

// Read the fixed helper's exact schema. Duplicate or case-folded field names
// cannot disguise a different mode, deadline, or positive-key capability.
func pdUnkeyReadiness(line []byte, durationMS int) (int64, error) {
	if len(line) == 0 || len(line) > 256 {
		return 0, errors.New("invalid unkey readiness size")
	}
	decoder := json.NewDecoder(bytes.NewReader(line))
	token, err := decoder.Token()
	if err != nil || token != json.Delim('{') {
		return 0, errors.New("invalid unkey readiness object")
	}
	fields := make(map[string]json.RawMessage)
	for decoder.More() {
		token, err = decoder.Token()
		key, ok := token.(string)
		if err != nil || !ok || fields[key] != nil || (key != "event" && key != "keys_transmitter" && key != "cleanup_due_ms") {
			return 0, errors.New("invalid unkey readiness field")
		}
		var value json.RawMessage
		if err := decoder.Decode(&value); err != nil {
			return 0, err
		}
		fields[key] = value
	}
	if token, err = decoder.Token(); err != nil || token != json.Delim('}') || decoder.Decode(new(any)) != io.EOF {
		return 0, errors.New("invalid unkey readiness ending")
	}
	var event string
	if json.Unmarshal(fields["event"], &event) != nil || string(fields["keys_transmitter"]) != "false" {
		return 0, errors.New("unkey guardian not ready")
	}
	if durationMS == 0 {
		if event != "ready" || len(fields) != 2 {
			return 0, errors.New("invalid lifetime guardian readiness")
		}
		return 0, nil
	}
	var dueText string
	if durationMS < 1 || durationMS > 15000 || event != "bounded_ready" || len(fields) != 3 || json.Unmarshal(fields["cleanup_due_ms"], &dueText) != nil {
		return 0, errors.New("invalid bounded guardian readiness")
	}
	due, err := strconv.ParseInt(dueText, 10, 64)
	if err != nil || due <= 0 || strconv.FormatInt(due, 10) != dueText {
		return 0, errors.New("invalid bounded cleanup deadline")
	}
	now, err := pdMonotonicMS()
	if err != nil || now <= 0 || due <= now || due-now > int64(durationMS) {
		return 0, errors.New("bounded cleanup deadline expired or excessive")
	}
	return due, nil
}

// A per-test owner, separate from the DSP's session-lifetime guardian. Closing
// it requests early cleanup; neither close, helper exit nor ACK proves RF idle.
// The caller owns the prepared API descriptor and must close its parent copy.
// A nonnil result with an error retains a child still awaiting termination;
// keep it until guard.done and refuse another test in the meantime.
type pdBoundedUnkey struct {
	guard   *aetherPDUnkey
	life    *os.File
	once    sync.Once
	retired atomic.Bool
}

func startPDBoundedUnkey(ctx context.Context, program string, api *os.File, client, owner uint32, durationMS int) (*pdBoundedUnkey, error) {
	if durationMS < 1 || durationMS > 15000 {
		return nil, errors.New("invalid bounded unkey duration")
	}
	read, write, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	defer read.Close()
	bounded := &pdBoundedUnkey{life: write}
	stopCancel := context.AfterFunc(ctx, bounded.close)
	guard, err := launchAetherPDUnkey(ctx, program, api, read, client, owner, durationMS)
	if err != nil && guard == nil {
		stopCancel()
		bounded.close()
		return nil, err
	}
	bounded.guard = guard
	go func() {
		<-guard.done
		bounded.close()
		stopCancel()
	}()
	if err != nil {
		bounded.close()
		return bounded, err
	}
	if ctx.Err() != nil || !bounded.observing() {
		bounded.close()
		return bounded, errors.New("bounded unkey retired during startup")
	}
	return bounded, nil
}

func (b *pdBoundedUnkey) close() {
	b.once.Do(func() {
		b.retired.Store(true)
		_ = b.life.Close()
	})
}

// Observation availability is one condition for an original-operation TX
// permit, never a grant. It cannot renew or certify release of that operation.
func (b *pdBoundedUnkey) observing() bool {
	if b.retired.Load() {
		return false
	}
	select {
	case <-b.guard.done:
		b.close()
		return false
	default:
	}
	now, err := pdMonotonicMS()
	if err != nil || now <= 0 || now >= b.guard.cleanupDue {
		b.close()
		return false
	}
	return !b.retired.Load()
}
