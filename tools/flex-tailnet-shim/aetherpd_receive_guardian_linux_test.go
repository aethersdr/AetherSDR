//go:build linux

package main

import (
	"context"
	"encoding/json"
	"errors"
	"golang.org/x/sys/unix"
	"os"
	"strings"
	"testing"
	"time"
)

func TestPDReceiveGuardianRetainsModeledSnapshot(t *testing.T) {
	request, _, _ := receiveRecoveryFixture()
	original := request.original
	original.settings.agcThreshold = 65
	original.settings.agcOffLevel = 10
	original.settings.nbLevel = 50
	data, err := encodePDReceiveGuardian(original)
	if err != nil {
		t.Fatal(err)
	}
	retained, err := decodePDReceiveGuardian(data)
	if err != nil {
		t.Fatal(err)
	}
	if !retained.complete || retained.settings != original.settings || retained.guiHandle != original.guiHandle || retained.panID != original.panID || retained.panGain != original.panGain || retained.clientID != original.clientID || retained.sliceID != original.sliceID {
		t.Fatal("guardian lost modeled receive settings or ownership")
	}
	var record pdReceiveGuardianRecord
	if err := json.Unmarshal(data, &record); err != nil {
		t.Fatal(err)
	}
	for _, bad := range [][]byte{nil, []byte(strings.Repeat("x", 4097)), append(append([]byte{}, data...), data...), append([]byte(" "), data...), []byte(strings.Replace(string(data), `"Version":1`, `"Version":1,"Version":1`, 1)), []byte(strings.Replace(string(data), `"Version":1`, `"Version":1,"command":"xmit 1"`, 1))} {
		if _, err := decodePDReceiveGuardian(bad); err == nil {
			t.Fatal("malformed, duplicate or command-bearing snapshot accepted")
		}
	}
	for _, change := range []func(*pdReceiveGuardianRecord){
		func(r *pdReceiveGuardianRecord) { r.GUIHandle++ },
		func(r *pdReceiveGuardianRecord) { r.PanID++ },
		func(r *pdReceiveGuardianRecord) { r.Version = 2 },
		func(r *pdReceiveGuardianRecord) {
			r.SliceStatus = strings.Replace(r.SliceStatus, "mode=USB", "mode=PD", 1)
		},
		func(r *pdReceiveGuardianRecord) {
			r.SliceStatus = strings.Replace(r.SliceStatus, "nb_level=50", "nb_level=101", 1)
		},
		func(r *pdReceiveGuardianRecord) { r.SliceStatus += "\nxmit 1" },
	} {
		bad := record
		change(&bad)
		data, err := json.Marshal(bad)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := decodePDReceiveGuardian(data); err == nil {
			t.Fatal("invalid settings or ownership snapshot accepted")
		}
	}
}

func TestPDReceiveGuardianLifetimeIgnoresFullPipe(t *testing.T) {
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	defer w.Close()
	writeFD := int(w.Fd())
	if err := unix.SetNonblock(writeFD, true); err != nil {
		t.Fatal(err)
	}
	block := make([]byte, 4096)
	for {
		_, err := unix.Write(writeFD, block)
		if err == unix.EAGAIN {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
	}
	done := make(chan error, 1)
	go func() { done <- waitPDReceiveGuardianLifetime(r) }()
	select {
	case err := <-done:
		t.Fatal("payload authorized cleanup", err)
	case <-time.After(30 * time.Millisecond):
	}
	w.Close()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("queued payload prevented lifetime hangup")
	}
}

func TestPDReceiveGuardianWaitRejectsDAXCollision(t *testing.T) {
	original, view := receiveRestoreFixture()
	view.slices[1].daxChannel = original.settings.daxChannel
	ctx, cancel := context.WithTimeout(context.Background(), 250*time.Millisecond)
	defer cancel()
	calls := 0
	err := waitPDReceiveGuardianRestore(ctx, original, func(context.Context, pdReceiveOriginal) (pdReceiveView, error) { calls++; return view, nil })
	if !errors.Is(err, errPDReceiveDAXOccupied) || calls != 1 || ctx.Err() != nil {
		t.Fatalf("DAX conflict was retried or obscured: calls=%d err=%v context=%v", calls, err, ctx.Err())
	}
}
func TestPDReceiveGuardianWaitAllowsModeTransition(t *testing.T) {
	original, view := receiveRestoreFixture()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	calls := 0
	err := waitPDReceiveGuardianRestore(ctx, original, func(context.Context, pdReceiveOriginal) (pdReceiveView, error) {
		calls++
		out := view
		if calls == 1 {
			out.slices[original.sliceID].mode = "PD"
		}
		return out, nil
	})
	if err != nil || calls != 2 {
		t.Fatalf("legitimate USB transition refused: calls=%d err=%v", calls, err)
	}
}
func TestPDReceiveGuardianWaitHonorsCancellation(t *testing.T) {
	original, _ := receiveRestoreFixture()
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	calls := 0
	err := waitPDReceiveGuardianRestore(ctx, original, func(context.Context, pdReceiveOriginal) (pdReceiveView, error) { calls++; return pdReceiveView{}, nil })
	if !errors.Is(err, context.Canceled) || calls != 0 {
		t.Fatalf("canceled observer ran: calls=%d err=%v", calls, err)
	}
}
