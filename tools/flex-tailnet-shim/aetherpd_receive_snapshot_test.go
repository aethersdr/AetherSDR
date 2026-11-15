package main

import (
	"os"
	"reflect"
	"strings"
	"testing"
)

// Required fields and spellings follow the captured FLEX-8600 4.3.16 slice
// status. Recipient and GUI identifiers are synthetic; no live connection.
const pdReceiveSnapshotFixture = "SABCDEF|slice 0 in_use=1 sample_rate=24000 RF_frequency=7.150000 client_handle=0x11223344 pan=0x40000000 mode=USB rxant=RX_A filter_lo=100 filter_hi=2800 agc_mode=med agc_threshold=50 agc_off_level=50 audio_level=50 audio_pan=50 audio_mute=0 dax=1 rit_on=0 rit_freq=0 squelch=1 squelch_level=20 nr=0 nr_level=50 nb=0 nb_level=0 anf=0 apf=0 apf_level=30"

func TestPDReceiveSnapshotCapturesCompleteRadioState(t *testing.T) {
	r, _, _ := receiveRecoveryFixture()
	got, err := capturePDReceiveOriginal(pdReceiveSnapshotFixture, r.original.guiHandle, r.original.clientID, r.original.panID, 0)
	if err != nil {
		t.Fatal(err)
	}
	r.original.recreateSafe = false
	r.original.recreateStep = 0
	if !reflect.DeepEqual(got, r.original) {
		t.Fatalf("captured settings differ: %+v", got)
	}
}

func TestPDReceiveSnapshotRequiresEveryCapturedField(t *testing.T) {
	r, _, _ := receiveRecoveryFixture()
	fields := strings.Fields(strings.SplitN(pdReceiveSnapshotFixture, "|slice 0 ", 2)[1])
	for _, field := range fields {
		name := strings.SplitN(field, "=", 2)[0]
		t.Run(name, func(t *testing.T) {
			broken := strings.Replace(pdReceiveSnapshotFixture, " "+field, "", 1)
			if _, err := capturePDReceiveOriginal(broken, r.original.guiHandle, r.original.clientID, r.original.panID, 0); err == nil {
				t.Fatal("partial radio state accepted")
			}
		})
	}
}

func TestPDReceiveSnapshotRejectsMalformedWire(t *testing.T) {
	r, _, _ := receiveRecoveryFixture()
	broken := []string{
		pdReceiveSnapshotFixture + " audio_level=0",
		pdReceiveSnapshotFixture + " extra=1 extra=2",
		pdReceiveSnapshotFixture + "|slice 1 removed",
		strings.Replace(pdReceiveSnapshotFixture, "0x11223344", "0x+1223344", 1),
		pdReceiveSnapshotFixture + "\nxmit 1", pdReceiveSnapshotFixture + "\r", pdReceiveSnapshotFixture + "\x00",
		strings.Replace(pdReceiveSnapshotFixture, "slice 0", "slice -1", 1),
		strings.Replace(pdReceiveSnapshotFixture, "slice 0", "slice 16", 1),
		strings.Replace(pdReceiveSnapshotFixture, "in_use=1", "in_use=0", 1),
		strings.Replace(pdReceiveSnapshotFixture, "client_handle=0x11223344", "client_handle=0x99887766", 1),
		strings.Replace(pdReceiveSnapshotFixture, "pan=0x40000000", "pan=0x40000001", 1),
		strings.Replace(pdReceiveSnapshotFixture, "RF_frequency=7.150000", "RF_frequency=NaN", 1),
		strings.Replace(pdReceiveSnapshotFixture, "audio_mute=0", "audio_mute=off", 1),
		strings.Replace(pdReceiveSnapshotFixture, "agc_threshold=50", "agc_threshold=+50", 1),
		strings.Replace(pdReceiveSnapshotFixture, "mode=USB", "mode=PD", 1),
		strings.Replace(pdReceiveSnapshotFixture, "sample_rate=24000", "sample_rate=192000", 1),
		"S1|slice 0 " + strings.Repeat("x", 4096), "R1|0|slice 0",
	}
	for i, line := range broken {
		if _, err := capturePDReceiveOriginal(line, r.original.guiHandle, r.original.clientID, r.original.panID, 0); err == nil {
			t.Errorf("malformed case%d accepted", i)
		}
	}
}

func TestPDReceiveFrequencyUsesExactHz(t *testing.T) {
	for text, want := range map[string]int64{"7.150000": 7150000, "7.150001": 7150001, "0.000001": 1, "7": 7000000, "7.15": 7150000, "1000000": 1_000_000_000_000} {
		got, err := pdReceiveFrequencyHz(text)
		if err != nil || got != want {
			t.Errorf("%s: %d %v", text, got, err)
		}
	}
	for _, text := range []string{"", "0", "7.", ".15", "+7.15", "-7.15", "7.1500001", "7e0", "7.15.0", "1000000.000001", "999999999999999999999999999999999"} {
		if _, err := pdReceiveFrequencyHz(text); err == nil {
			t.Error("invalid MHz admitted", text)
		}
	}
}

func TestPDReceiveSnapshotCapturedFirmwareFrame(t *testing.T) {
	data, err := os.ReadFile("testdata/aetherpd/receive_slice_full.txt")
	if err != nil {
		t.Fatal(err)
	}
	r, _, _ := receiveRecoveryFixture()
	got, err := capturePDReceiveOriginal(strings.TrimSuffix(string(data), "\n"), r.original.guiHandle, r.original.clientID, r.original.panID, 0)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got, r.original) {
		t.Fatalf("captured firmware state mismatch: %+v", got)
	}
}
