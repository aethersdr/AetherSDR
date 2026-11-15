package main

import (
	"bytes"
	"io"
	"os"
	"strings"
	"testing"
	"time"
)

type pdObservationWire struct {
	*strings.Reader
	writes bytes.Buffer
}

func (wire *pdObservationWire) Write(p []byte) (int, error) { return wire.writes.Write(p) }

func pdObservationFrames(t *testing.T) string {
	t.Helper()
	slice, err := os.ReadFile("testdata/aetherpd/receive_slice_full.txt")
	if err != nil {
		t.Fatal(err)
	}
	return "V1.4.0.0\nH01020304\n" +
		"S01020304|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1 amplifier=\n" +
		"S01020304|client 0x11223344 connected local_ptt=1 client_id=11111111-2222-4333-8444-555555555555 program=AetherSDR station=Automation\nR1|0|\n" +
		"S01020304|display pan 0x40000000 client_handle=0x11223344 center=7.150000 bandwidth=0.075170 rfgain=0 rxant=RX_A\nR2|0|\n" +
		strings.TrimSpace(string(slice)) + "\n" +
		"S01020304|slice 1 in_use=1 client_handle=0x55667788 pan=0x40000001 RF_frequency=7.175000 mode=LSB dax=2\nR3|0|\nR4|0|\n"
}

func TestPDReceiveObserverSnapshot(t *testing.T) {
	frames := pdObservationFrames(t)
	// Real pan/slice deltas must preserve prior identity and untouched fields.
	frames = strings.Replace(frames, "R3|0|", "S0|display pan 0x40000000 min_dbm=-160 rfgain=0\nS0|slice 0 audio_level=42\nR3|0|", 1)
	wire := &pdObservationWire{Reader: strings.NewReader(frames)}
	stamp := time.Unix(100, 0)
	observation, err := readPDReceiveObservation(wire, 0x11223344, 0x40000000, 0, func() time.Time { return stamp })
	if err != nil {
		t.Fatal(err)
	}
	if wire.writes.String() != pdReceiveSubscriptions {
		t.Fatalf("unexpected commands: %q", wire.writes.String())
	}
	if observation.view.complete || !observation.original.complete {
		t.Fatal("observation must capture original without admitting recovery")
	}
	if observation.original.settings.audioLevel != 42 || observation.original.settings.agcThreshold != 50 || !observation.view.observedAt.Equal(stamp) || !observation.view.ready || !observation.view.txAllowed {
		t.Fatalf("incorrect original or readiness: %+v", observation)
	}
	if observation.view.slices[1].owner != 0x55667788 || observation.view.slices[1].daxChannel != 2 || observation.view.panLowHz != 7112415 || observation.view.panHighHz != 7187585 {
		t.Fatalf("incorrect inventory or coverage: %+v", observation.view)
	}
}

func TestPDReceiveObserverRejectsIncompleteAndReusedState(t *testing.T) {
	baseline := pdObservationFrames(t)
	cases := map[string]string{
		"missing client reply":     strings.Replace(baseline, "R1|0|\n", "", 1),
		"failed subscription":      strings.Replace(baseline, "R2|0|", "R2|50000001|", 1),
		"duplicate reply":          strings.Replace(baseline, "R3|0|", "R2|0|", 1),
		"missing GUI identity":     strings.Replace(baseline, "client_id=11111111-2222-4333-8444-555555555555", "unused=1", 1),
		"missing slice identity":   strings.Replace(baseline, "client_handle=0x55667788", "unused=0x55667788", 1),
		"missing original setting": strings.Replace(baseline, "agc_threshold=50", "unused_threshold=50", 1),
		"duplicate attribute":      strings.Replace(baseline, "audio_level=50", "audio_level=50 audio_level=50", 1),
		"overlong frame":           strings.Repeat("x", 4097) + "\n" + baseline,
		"bad header":               strings.Replace(baseline, "S01020304|interlock", "Sxyz|interlock", 1),
		"bad control byte":         strings.Replace(baseline, "state=READY", "state=READY\x00", 1),
		"pan antenna mismatch":     strings.Replace(baseline, "rfgain=0 rxant=RX_A", "rfgain=0 rxant=ANT1", 1),
	}
	for name, delta := range map[string]string{
		"GUI disconnect":             "client 0x11223344 disconnected",
		"GUI reused":                 "client 0x11223344 connected client_id=99999999-2222-4333-8444-555555555555",
		"pan removed":                "display pan 0x40000000 removed",
		"pan reused":                 "display pan 0x40000000 client_handle=0x55667788",
		"slice removed":              "slice 0 in_use=0",
		"slice reused":               "slice 0 client_handle=0x55667788",
		"slice removed then partial": "slice 0 removed\nS0|slice 0 audio_level=50",
		"inventory overflow":         "slice 16 in_use=1",
	} {
		cases[name] = strings.Replace(baseline, "R4|0|", "S0|"+delta+"\nR4|0|", 1)
	}
	for name, frames := range cases {
		t.Run(name, func(t *testing.T) {
			wire := &pdObservationWire{Reader: strings.NewReader(frames)}
			if _, err := readPDReceiveObservation(wire, 0x11223344, 0x40000000, 0, time.Now); err == nil {
				t.Fatal("accepted incomplete or changed snapshot")
			}
		})
	}
}

type pdObservationShortWrite struct{ io.Reader }

func (pdObservationShortWrite) Write(p []byte) (int, error) { return len(p) - 1, nil }

func TestPDReceiveObserverShortWrite(t *testing.T) {
	wire := pdObservationShortWrite{strings.NewReader(pdObservationFrames(t))}
	if _, err := readPDReceiveObservation(wire, 0x11223344, 0x40000000, 0, time.Now); err == nil {
		t.Fatal("ignored short subscription write")
	}
}

func TestPDReceiveObserverMissingLiveInterlockDoesNotInventReady(t *testing.T) {
	for _, replacement := range []string{"unused=READY", ""} {
		frames := strings.Replace(pdObservationFrames(t), "state=READY", replacement, 1)
		wire := &pdObservationWire{Reader: strings.NewReader(frames)}
		if _, err := readPDReceiveObservation(wire, 0x11223344, 0x40000000, 0, time.Now); err == nil {
			t.Fatal("missing live interlock accepted")
		}
	}
}

func TestPDReceiveObserverWaitsForLiveInterlockAfterReplies(t *testing.T) {
	frames := strings.Replace(pdObservationFrames(t), "state=READY", "unused=READY", 1)
	frames += "S0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1 amplifier=\n"
	wire := &pdObservationWire{Reader: strings.NewReader(frames)}
	calls := 0
	observation, err := readPDReceiveObservation(wire, 0x11223344, 0x40000000, 0, func() time.Time { calls++; return time.Unix(100+int64(calls), 0) })
	if err != nil {
		t.Fatal(err)
	}
	if !observation.interlockComplete || !observation.view.ready || observation.view.complete || observation.view.observedAt != time.Unix(101, 0) {
		t.Fatal("late interlock lost or timestamp refreshed over older inventory")
	}
}

func TestPDReceiveObserverWaveformMetadataDEL(t *testing.T) {
	frames := "S0|waveform installed_list=AetherHealth\x7f\x7fv0.1.0,NereusNativeProbe\x7f\x7fv0.1.2\n" + pdObservationFrames(t)
	wire := &pdObservationWire{Reader: strings.NewReader(frames)}
	if _, err := readPDReceiveObservation(wire, 0x11223344, 0x40000000, 0, time.Now); err != nil {
		t.Fatal(err)
	}
}

func TestPDReceiveObserverCapturedFirmwareFrames(t *testing.T) {
	frames, err := os.ReadFile("testdata/aetherpd/receive_observer_full.txt")
	if err != nil {
		t.Fatal(err)
	}
	wire := &pdObservationWire{Reader: strings.NewReader(string(frames))}
	if _, err := readPDReceiveObservation(wire, 0x11223344, 0x40000000, 0, time.Now); err == nil {
		t.Fatal("inventory-only capture incorrectly accepted as full live observation")
	}
	frames = append(frames, []byte("S0|interlock tx_client_handle=0x00000000 state=READY reason= source= tx_allowed=1 amplifier=\n")...)
	wire = &pdObservationWire{Reader: strings.NewReader(string(frames))}
	observation, err := readPDReceiveObservation(wire, 0x11223344, 0x40000000, 0, time.Now)
	if err != nil {
		t.Fatal(err)
	}
	if !observation.original.complete || observation.original.settings.frequencyHz != 7150000 || !observation.view.slices[1].present || !observation.interlockComplete || !observation.view.ready || !observation.view.complete {
		t.Fatalf("incorrect captured observation: %+v", observation)
	}
}

func TestPDReceiveObserverSelectsOnlyActiveOwnedReceiver(t *testing.T) {
	frames := pdObservationFrames(t)
	for name, change := range map[string]string{
		"selected":       "",
		"no active":      "active=0",
		"foreign active": "active=1 client_handle=0x55667788",
		"two active":     "active=1 client_handle=0x11223344",
	} {
		t.Run(name, func(t *testing.T) {
			text := frames
			if change != "" {
				text = strings.Replace(text, "R3|0|", "S0|slice 1 "+change+"\nR3|0|", 1)
			}
			if name == "no active" {
				text = strings.Replace(text, " active=1 ", " active=0 ", 1)
			}
			wire := &pdObservationWire{Reader: strings.NewReader(text)}
			got, err := readPDReceiveObservation(wire, 0x11223344, 0, -1, time.Now)
			if name == "no active" || name == "two active" {
				if err == nil {
					t.Fatal("ambiguous or missing active receiver accepted")
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
			if got.original.sliceID != 0 || got.original.panID != 0x40000000 {
				t.Fatal("wrong active receiver")
			}
		})
	}
}

func TestPDReceiveWorldObservesPDOrAbsentSliceWithoutReplacingOriginal(t *testing.T) {
	for _, missing := range []bool{false, true} {
		frames := strings.Replace(pdObservationFrames(t), " mode=USB ", " mode=PD ", 1)
		if missing {
			frames = strings.Replace(frames, "R4|0|", "S0|slice 0 in_use=0\nR4|0|", 1)
		}
		wire := &pdObservationWire{Reader: strings.NewReader(frames)}
		observation, err := readPDReceiveState(wire, 0x11223344, 0x40000000, 0, time.Now, false)
		if err != nil {
			t.Fatal(err)
		}
		if observation.original.complete || observation.view.complete || observation.view.slices[0].present == missing || !missing && observation.view.slices[0].mode != "PD" {
			t.Fatal("world snapshot invented original settings or lost current receiver state")
		}
	}
}
