package main

import (
	"reflect"
	"testing"
	"time"
)

func receiveRecoveryFixture() (pdReceiveRecoveryRequest, pdReceiveView, time.Time) {
	now := time.Unix(1_700_000_000, 0)
	original := pdReceiveOriginal{complete: true, recreateSafe: true, recreateStep: 100, guiHandle: 0x11223344, clientID: "11111111-2222-4333-8444-555555555555", sliceID: 0, panID: 0x40000000, panGain: 0,
		settings: pdReceiveSettings{mode: "USB", frequencyHz: 7150000, antenna: "RX_A", filterLow: 100, filterHigh: 2800, agcMode: "med", agcThreshold: 50, agcOffLevel: 50, audioLevel: 50, audioPan: 50, daxChannel: 1, squelch: true, squelchLevel: 20, nrLevel: 50, apfLevel: 30}}
	request := pdReceiveRecoveryRequest{original: original, pdSliceID: 0, pdOwner: original.guiHandle, pdPan: original.panID, pdObserved: true, apiLeaseFailed: true, operatorRequested: true}
	view := pdReceiveView{complete: true, remainingSlices: 1, observedAt: now, guiHandle: original.guiHandle, clientID: original.clientID, guiConnected: true, panID: original.panID, panOwner: original.guiHandle, panAntenna: "RX_A", panGain: 0, panLowHz: 7050000, panHighHz: 7250000, maxSlices: 2, maxDAX: 2, ready: true, txAllowed: true,
		slices: [16]pdReceiveSlice{1: {present: true, owner: 99, panID: 0x40000001, frequencyHz: 7175000, mode: "LSB"}}}
	return request, view, now
}

func TestPDReceiveRecoveryRequiresIntentAndFreshOwnership(t *testing.T) {
	cases := []struct {
		name  string
		alter func(*pdReceiveRecoveryRequest, *pdReceiveView)
	}{
		{"no operator request", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { r.operatorRequested = false }},
		{"not API failure", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { r.apiLeaseFailed = false }},
		{"never selected PD", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { r.pdObserved = false }},
		{"different PD slice", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { r.pdSliceID = 1 }},
		{"foreign PD owner", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { r.pdOwner++ }},
		{"different PD pan", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { r.pdPan++ }},
		{"incomplete original", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { r.original.complete = false }},
		{"unsafe original mode", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { r.original.settings.mode = "PD" }},
		{"antenna injection", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { r.original.settings.antenna = "RX_A\nxmit 1" }},
		{"invalid frequency", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { r.original.settings.frequencyHz = -1 }},
		{"invalid filter", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { r.original.settings.filterLow = 3000 }},
		{"invalid audio", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { r.original.settings.audioLevel = 101 }},
		{"client injection", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { r.original.clientID = "bad\nclient gui" }},
		{"incomplete world", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { v.complete = false }},
		{"clock reversal", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { v.observedAt = v.observedAt.Add(time.Nanosecond) }},
		{"stale world", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) {
			v.observedAt = v.observedAt.Add(-time.Second - time.Nanosecond)
		}},
		{"GUI gone", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { v.guiConnected = false }},
		{"handle reused", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) {
			v.clientID = "11111111-1111-1111-1111-111111111111"
		}},
		{"handle changed", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { v.guiHandle++ }},
		{"transmitting", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) {
			v.ready = false
			v.source = "SW"
			v.txOwner = r.original.guiHandle
		}},
		{"TX not allowed", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { v.txAllowed = false }},
		{"inhibit reason", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { v.reason = "TIMEOUT" }},
		{"foreign pan", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { v.panOwner++ }},
		{"pan reused", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { v.panID++ }},
		{"antenna changed", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { v.panAntenna = "ANT2" }},
		{"SCU gain changed", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { v.panGain = 10 }},
		{"frequency off pan", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { v.panLowHz = 7200000 }},
		{"ID occupied by same GUI", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { v.slices[0] = v.slices[1] }},
		{"ID occupied by foreign GUI", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) {
			v.slices[0] = pdReceiveSlice{present: true, owner: 99, panID: 0x40000000}
		}},
		{"DAX reassigned", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) {
			s := v.slices[1]
			s.owner = 99
			s.daxChannel = 1
			v.slices[1] = s
		}},
		{"DAX unavailable", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { v.maxDAX = 0 }},
		{"invalid slot inventory", func(r *pdReceiveRecoveryRequest, v *pdReceiveView) { v.slices[5] = v.slices[1] }},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			r, v, now := receiveRecoveryFixture()
			c.alter(&r, &v)
			if _, err := planPDReceiveRecovery(r, v, now); err == nil {
				t.Fatal("unsafe recovery admitted")
			}
		})
	}
}

func TestPDReceiveRecoveryRefusesAllocationRaces(t *testing.T) {
	cases := []struct {
		name  string
		alter func(*pdReceiveCreated, *pdReceiveView)
	}{
		{"no create reply", func(c *pdReceiveCreated, v *pdReceiveView) { c.acknowledged = false }},
		{"different allocated ID", func(c *pdReceiveCreated, v *pdReceiveView) { c.sliceID = 1 }},
		{"DAX changed on created receiver", func(c *pdReceiveCreated, v *pdReceiveView) { x := v.slices[0]; x.daxChannel = 2; v.slices[0] = x }},
		{"created ID disappeared", func(c *pdReceiveCreated, v *pdReceiveView) { v.slices[0] = pdReceiveSlice{} }},
		{"created ID reused", func(c *pdReceiveCreated, v *pdReceiveView) { s := v.slices[0]; s.owner = 99; v.slices[0] = s }},
		{"created pan changed", func(c *pdReceiveCreated, v *pdReceiveView) { s := v.slices[0]; s.panID++; v.slices[0] = s }},
		{"operator tuned receiver", func(c *pdReceiveCreated, v *pdReceiveView) { s := v.slices[0]; s.frequencyHz++; v.slices[0] = s }},
		{"operator changed mode", func(c *pdReceiveCreated, v *pdReceiveView) { s := v.slices[0]; s.mode = "LSB"; v.slices[0] = s }},
		{"PTT after create", func(c *pdReceiveCreated, v *pdReceiveView) { v.ready = false; v.source = "SW" }},
		{"stale after create", func(c *pdReceiveCreated, v *pdReceiveView) { v.observedAt = v.observedAt.Add(-2 * time.Second) }},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			r, v, now := receiveRecoveryFixture()
			plan, err := planPDReceiveRecovery(r, v, now)
			if err != nil {
				t.Fatal(err)
			}
			v.slices[0] = pdReceiveSlice{present: true, owner: r.original.guiHandle, panID: r.original.panID, frequencyHz: 7150000, mode: "USB"}
			created := pdReceiveCreated{acknowledged: true, sliceID: 0}
			c.alter(&created, &v)
			if _, err = confirmPDReceiveRecovery(plan, created, v, now); err == nil {
				t.Fatal("allocation race admitted")
			}
		})
	}
}

func TestPDReceiveRecoveryKeepsCapturedSettings(t *testing.T) {
	r, v, now := receiveRecoveryFixture()
	foreign := v.slices[1]
	foreign.owner = 99
	v.slices[1] = foreign
	plan, err := planPDReceiveRecovery(r, v, now)
	if err != nil {
		t.Fatal(err)
	}
	want := r.original.settings
	r.original.settings.frequencyHz = 7200000
	v.slices[0] = pdReceiveSlice{present: true, owner: plan.original.guiHandle, panID: plan.original.panID, frequencyHz: 7150000, mode: "USB"}
	got, err := confirmPDReceiveRecovery(plan, pdReceiveCreated{acknowledged: true, sliceID: 0}, v, now)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got, want) || v.slices[1] != foreign {
		t.Fatal("captured settings or unrelated receiver changed")
	}
	v.observedAt = now.Add(-time.Second)
	if _, err = confirmPDReceiveRecovery(plan, pdReceiveCreated{acknowledged: true, sliceID: 0}, v, now); err != nil {
		t.Fatal("freshness boundary rejected", err)
	}
}

func TestPDReceiveRecoveryAdmissionExpires(t *testing.T) {
	r, v, now := receiveRecoveryFixture()
	plan, err := planPDReceiveRecovery(r, v, now)
	if err != nil {
		t.Fatal(err)
	}
	v.slices[0] = pdReceiveSlice{present: true, owner: r.original.guiHandle, panID: r.original.panID, frequencyHz: 7150000, mode: "USB"}
	later := now.Add(5*time.Second + time.Nanosecond)
	v.observedAt = later
	if _, err = confirmPDReceiveRecovery(plan, pdReceiveCreated{acknowledged: true, sliceID: 0}, v, later); err == nil {
		t.Fatal("expired allocation admitted with fresh radio state")
	}
}
