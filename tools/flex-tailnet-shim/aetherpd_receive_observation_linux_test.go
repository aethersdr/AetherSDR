//go:build linux

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestPDStatusRecordsReceiveOwnershipBeforeCalibration(t *testing.T) {
	p, peer := privateStatusFixture(t)
	j, view, _ := receiveJournalFixture()
	p.receiveJournal = j
	var calls int
	p.receiveObserve = func(ctx context.Context, original pdReceiveOriginal) (pdReceiveView, error) {
		calls++
		if original != j.original {
			t.Fatal("ownership observation changed original identity")
		}
		deadline, ok := ctx.Deadline()
		if !ok || time.Until(deadline) > 750*time.Millisecond {
			t.Fatal("ownership observation has no bounded status budget")
		}
		view.observedAt = time.Now()
		return view, nil
	}
	var fields map[string]any
	if err := json.Unmarshal(pdFixture(t, "cold_start"), &fields); err != nil {
		t.Fatal(err)
	}
	fields["active_slice"] = 0
	fields["keyed"] = false
	fields["correcting"] = false
	fields["calibration_control"] = true
	fields["calibration_state"] = "idle"
	fields["calibration_samples_left"] = 0
	data, err := json.Marshal(fields)
	if err != nil {
		t.Fatal(err)
	}
	complete := make(chan error, 1)
	go func() {
		request := make([]byte, 9)
		n, _, _, _, err := peer.ReadMsgUnix(request, nil)
		if err == nil && (n != 9 || request[0] != '?') {
			err = errors.New("unexpected native query")
		}
		if err == nil {
			_, err = peer.Write(append(request[1:], data...))
		}
		complete <- err
	}()
	status, err := p.queryStatus(context.Background())
	if err != nil || !status.Available || status.Keyed {
		t.Fatal("fresh idle native status failed", status, err)
	}
	if err := <-complete; err != nil {
		t.Fatal(err)
	}
	if calls != 1 {
		t.Fatalf("status did not independently witness PD before arming: %d calls", calls)
	}
	j.recordExit(true)
	view.slices[0] = pdReceiveSlice{}
	view.observedAt = time.Now()
	if _, err := j.beginExistingRestore(); err != nil {
		t.Fatal("pre-arm API failure cannot begin explicit recovery", err)
	}
	defer j.endRestore()
	if _, err := planPDReceiveRecovery(j.recoveryRequest(true), view, time.Now()); err != nil {
		t.Fatal("pre-arm API failure cannot recover its missing receiver", err)
	}
	if _, err := planPDReceiveRecovery(j.recoveryRequest(false), view, time.Now()); err == nil {
		t.Fatal("ownership witness manufactured operator restore intent")
	}
}

func TestPDReceiveObservationRequiresNativeTriggerAndFreshOwnedRadioState(t *testing.T) {
	for _, invalid := range []string{"unavailable", "unregistered", "keyed", "missing slice", "wrong slice", "non-PD slice", "foreign", "stale", "transport", "exit during observe", "canceled"} {
		t.Run(invalid, func(t *testing.T) {
			j, view, _ := receiveJournalFixture()
			zero := 0
			status := aetherPDStatus{Available: true, Registered: true, nativeActiveSlice: &zero}
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			switch invalid {
			case "unavailable":
				status.Available = false
			case "unregistered":
				status.Registered = false
			case "keyed":
				status.Keyed = true
			case "missing slice":
				status.nativeActiveSlice = nil
			case "wrong slice":
				zero = 1
			case "non-PD slice":
				zero = -1
			case "foreign":
				view.slices[0].owner++
			case "stale":
				view.observedAt = time.Now().Add(-2 * time.Second)
			case "canceled":
				cancel()
			}
			p := &aetherPDProcess{receiveJournal: j, receiveObserve: func(context.Context, pdReceiveOriginal) (pdReceiveView, error) {
				if invalid == "transport" {
					return pdReceiveView{}, errors.New("fixture lost radio")
				}
				if invalid == "exit during observe" {
					j.recordExit(true)
				}
				return view, nil
			}}
			p.observePDReceiveOwnership(ctx, status)
			j.recordExit(true)
			if j.recoveryRequest(true).pdObserved {
				t.Fatal("invalid status/radio observation authorized a recovery witness")
			}
		})
	}
}

func TestPDReceiveObservationSingleConcurrentClaimAndBoundedRetry(t *testing.T) {
	j, _, now := receiveJournalFixture()
	var entered atomic.Int32
	var group sync.WaitGroup
	for i := 0; i < 32; i++ {
		group.Add(1)
		go func() {
			defer group.Done()
			if _, ok := j.beginPDObservation(0, now); ok {
				entered.Add(1)
			}
		}()
	}
	group.Wait()
	if entered.Load() != 1 {
		t.Fatal("parallel status queries acquired multiple observations")
	}
	j.endPDObservation()
	if _, ok := j.beginPDObservation(0, now.Add(time.Second-time.Nanosecond)); ok {
		t.Fatal("failed witness retried without backoff")
	}
	if _, ok := j.beginPDObservation(0, now.Add(time.Second)); !ok {
		t.Fatal("failed witness cannot retry after bound")
	}
	j.endPDObservation()
	j.recordExit(true)
	if _, ok := j.beginPDObservation(0, now.Add(2*time.Second)); ok {
		t.Fatal("closed process acquired new ownership evidence")
	}
}

func TestPDNativeActiveSliceStaysPrivateAndRejectsMalformedValues(t *testing.T) {
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(pdFixture(t, "cold_start"), &fields); err != nil {
		t.Fatal(err)
	}
	for _, value := range []string{"null", "-2", "16", "0.5", `"0"`, "true"} {
		fields["active_slice"] = json.RawMessage(value)
		data, _ := json.Marshal(fields)
		if _, err := parseAetherPDStatus(data); err == nil {
			t.Fatalf("invalid active slice accepted: %s", value)
		}
	}
	fields["active_slice"] = json.RawMessage("0")
	data, _ := json.Marshal(fields)
	status, err := parseAetherPDStatus(data)
	if err != nil || status.nativeActiveSlice == nil || *status.nativeActiveSlice != 0 {
		t.Fatal("valid native trigger lost", err)
	}
	public, _ := json.Marshal(status)
	if bytes.Contains(public, []byte("active_slice")) || bytes.Contains(public, []byte("nativeActiveSlice")) {
		t.Fatal("private receiver hint crossed public status boundary")
	}
}
