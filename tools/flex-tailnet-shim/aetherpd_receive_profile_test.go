package main

import (
	"os"
	"strings"
	"testing"
	"time"
)

func TestPDReceiveFullProfileAndCapacity(t *testing.T) {
	data, err := os.ReadFile("testdata/aetherpd/receive_observer_full.txt")
	if err != nil {
		t.Fatal(err)
	}
	wire := &pdObservationWire{Reader: strings.NewReader(strings.TrimSpace(string(data)) + "\nS0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1 amplifier=\n")}
	o, err := readPDReceiveObservation(wire, 0x11223344, 0x40000000, 0, func() time.Time { return time.Now() })
	if err != nil {
		t.Fatal(err)
	}
	if !o.original.recreateSafe || o.original.recreateStep != 100 || !o.view.complete || o.view.remainingSlices != 2 {
		t.Fatalf("full observed resources/profile not captured: %+v", o)
	}
}

func TestPDReceiveRecreateRefusesUnknownOrChangedProfile(t *testing.T) {
	data, err := os.ReadFile("testdata/aetherpd/receive_slice_full.txt")
	if err != nil {
		t.Fatal(err)
	}
	fields := map[string]string{}
	for _, word := range strings.Fields(string(data)) {
		if k, v, ok := strings.Cut(word, "="); ok {
			fields[k] = v
		}
	}
	if _, safe := pdReceiveRecreateProfile(fields, "USB"); !safe {
		t.Fatal("measured basic profile refused")
	}
	for _, key := range []string{"step", "lock", "chain_mode", "nr", "nr_wlen", "xit_freq", "txant", "play", "diversity", "fm_tone_value", "esc_gain"} {
		t.Run(key, func(t *testing.T) {
			old := fields[key]
			delete(fields, key)
			if _, safe := pdReceiveRecreateProfile(fields, "USB"); safe {
				t.Fatal("missing profile field admitted")
			}
			fields[key] = "changed"
			if _, safe := pdReceiveRecreateProfile(fields, "USB"); safe {
				t.Fatal("changed unsupported profile admitted")
			}
			fields[key] = old
		})
	}
	if _, safe := pdReceiveRecreateProfile(fields, "FM"); safe {
		t.Fatal("unqualified mode admitted")
	}
}

func TestPDReceiveObserverRejectsInvalidCapacity(t *testing.T) {
	for _, value := range []string{"-1", "17", "unknown"} {
		frames := "S0|radio slices=" + value + "\n" + pdObservationFrames(t)
		wire := &pdObservationWire{Reader: strings.NewReader(frames)}
		if _, err := readPDReceiveObservation(wire, 0x11223344, 0x40000000, 0, time.Now); err == nil {
			t.Fatal("invalid capacity admitted", value)
		}
	}
}
