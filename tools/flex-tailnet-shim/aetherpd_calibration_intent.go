package main

import (
	"context"
	"errors"
	"sync"
)

var errPDCalibrationStopped = errors.New("AetherPD session is stopped")

type pdCalibrationArmIntent struct{ cancel context.CancelFunc }

// A pending arm spans observation and the native request. Cancel invalidates
// that intent before sending X, so an older snapshot cannot re-arm afterward.
type pdCalibrationIntents struct {
	mu      sync.Mutex
	pending *pdCalibrationArmIntent
	closed  bool
}

func (intents *pdCalibrationIntents) beginArm(parent context.Context) (context.Context, func(), error) {
	if err := parent.Err(); err != nil {
		return nil, nil, err
	}
	intents.mu.Lock()
	defer intents.mu.Unlock()
	if intents.closed {
		return nil, nil, errPDCalibrationStopped
	}
	if intents.pending != nil {
		return nil, nil, errors.New("arm request already pending")
	}
	ctx, cancel := context.WithCancel(parent)
	intent := &pdCalibrationArmIntent{cancel: cancel}
	intents.pending = intent
	var once sync.Once
	finish := func() {
		once.Do(func() {
			intents.mu.Lock()
			if intents.pending == intent {
				intents.pending = nil
			}
			intents.mu.Unlock()
			cancel()
		})
	}
	return ctx, finish, nil
}

func (intents *pdCalibrationIntents) cancelPendingArm() {
	intents.mu.Lock()
	pending := intents.pending
	intents.mu.Unlock()
	if pending != nil {
		pending.cancel()
	}
}

// Revoking session authority is permanent for this owned process. A later
// token or reconnect cannot revive an old calibration intent.
func (intents *pdCalibrationIntents) close() {
	intents.mu.Lock()
	intents.closed = true
	pending := intents.pending
	intents.mu.Unlock()
	if pending != nil {
		pending.cancel()
	}
}
