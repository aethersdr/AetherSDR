package main

import (
	"errors"
	"strings"
	"sync"
	"time"
)

// This journal is scoped to one owned process. It never performs recovery or
// converts process death or a missing slice into operator restore intent.
type pdReceiveJournal struct {
	mu                  sync.Mutex
	original            pdReceiveOriginal
	pdObserved          bool
	apiFailed           bool
	processDone         bool
	restoring           bool
	allocationAttempted bool
	allocationConfirmed bool
}

func (j *pdReceiveJournal) beginExistingRestore() (pdReceiveOriginal, error) {
	j.mu.Lock()
	defer j.mu.Unlock()
	if j.allocationAttempted && !j.allocationConfirmed || j.restoring || !j.processDone || !j.pdObserved || !j.original.complete {
		return pdReceiveOriginal{}, errors.New("closed owned PD session required for receive restoration")
	}
	j.restoring = true
	return j.original, nil
}

func (j *pdReceiveJournal) endRestore() {
	j.mu.Lock()
	defer j.mu.Unlock()
	j.restoring = false
}

func (j *pdReceiveJournal) recordPD(view pdReceiveView, now time.Time) error {
	j.mu.Lock()
	defer j.mu.Unlock()
	o := j.original
	if j.processDone || !o.complete || o.sliceID < 0 || o.sliceID >= len(view.slices) || view.observedAt.IsZero() || now.Before(view.observedAt) || now.Sub(view.observedAt) > time.Second {
		return errors.New("fresh owned PD observation required")
	}
	slice := view.slices[o.sliceID]
	if !view.guiConnected || view.guiHandle != o.guiHandle || !strings.EqualFold(view.clientID, o.clientID) || view.panID != o.panID || view.panOwner != o.guiHandle || view.panAntenna != o.settings.antenna || view.panGain != o.panGain || !slice.present || slice.owner != o.guiHandle || slice.panID != o.panID || slice.mode != "PD" || slice.frequencyHz != o.settings.frequencyHz {
		return errors.New("PD receiver differs from captured receive session")
	}
	j.pdObserved = true
	return nil
}

func (j *pdReceiveJournal) recordExit(apiFailed bool) {
	j.mu.Lock()
	defer j.mu.Unlock()
	if !j.processDone {
		j.processDone = true
		j.apiFailed = apiFailed
	}
}

func (j *pdReceiveJournal) recoveryRequest(operatorRequested bool) pdReceiveRecoveryRequest {
	j.mu.Lock()
	defer j.mu.Unlock()
	return pdReceiveRecoveryRequest{original: j.original, pdSliceID: j.original.sliceID, pdOwner: j.original.guiHandle, pdPan: j.original.panID, pdObserved: j.pdObserved, apiLeaseFailed: j.processDone && j.apiFailed, operatorRequested: operatorRequested}
}

// A missing ACK must never cause a second allocation on a later HTTP retry.
func (j *pdReceiveJournal) consumeAllocation() error {
	j.mu.Lock()
	defer j.mu.Unlock()
	if !j.restoring || !j.processDone || !j.apiFailed || !j.pdObserved || j.allocationAttempted {
		return errors.New("owned API failure allocation already attempted or unavailable")
	}
	j.allocationAttempted = true
	return nil
}

func (j *pdReceiveJournal) confirmAllocation() error {
	j.mu.Lock()
	defer j.mu.Unlock()
	if !j.restoring || !j.allocationAttempted || j.allocationConfirmed {
		return errors.New("allocation confirmation unavailable")
	}
	j.allocationConfirmed = true
	return nil
}

func (j *pdReceiveJournal) hasConfirmedAllocation() bool {
	j.mu.Lock()
	defer j.mu.Unlock()
	return j.allocationConfirmed
}
