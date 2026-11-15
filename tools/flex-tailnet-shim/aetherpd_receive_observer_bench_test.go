package main

import (
	"context"
	"encoding/json"
	"os"
	"testing"
	"time"
)

// Explicit bench opt-in; ordinary tests never open a radio connection.
func TestPDReceiveObserverRadio(t *testing.T) {
	if os.Getenv("AETHERPD_RECEIVE_OBSERVER_BENCH") != "1" {
		t.Skip("read-only radio bench probe not requested")
	}
	handle, err := pdReceiveHexHandle(os.Getenv("AETHERPD_RECEIVE_OBSERVER_GUI"))
	if err != nil {
		t.Fatal(err)
	}
	started := time.Now()
	observation, err := observePDReceive(context.Background(), handle, 0x40000000, 0)
	if err != nil {
		t.Fatal(err)
	}
	view, original := observation.view, observation.original
	if !original.complete || view.complete || !observation.interlockComplete || !view.ready || !view.txAllowed || view.txOwner != 0 || view.source != "" || view.reason != "" {
		t.Fatal("bench must remain idle; snapshot must not admit restoration")
	}
	data, err := json.Marshal(map[string]any{
		"elapsed_ms": time.Since(started).Milliseconds(), "oldest_observation_age_ms": time.Since(view.observedAt).Milliseconds(),
		"uid": os.Geteuid(), "slice_id": original.sliceID, "pan_id": original.panID, "gui_handle": original.guiHandle,
		"mode": original.settings.mode, "frequency_hz": original.settings.frequencyHz, "antenna": original.settings.antenna,
		"dax": original.settings.daxChannel, "other_slice_present": view.slices[1].present,
		"other_slice_hz": view.slices[1].frequencyHz, "live_interlock_complete": observation.interlockComplete, "ready": view.ready, "recovery_admitted": view.complete,
	})
	if err != nil {
		t.Fatal(err)
	}
	t.Log(string(data))
}
