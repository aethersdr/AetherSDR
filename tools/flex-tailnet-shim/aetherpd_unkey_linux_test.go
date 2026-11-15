//go:build linux

package main

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestPDPreparedClientHandles(t *testing.T) {
	for _, text := range []string{"0x00000001", "0xabcdefAB", "0xFFFFFFFF"} {
		if _, err := pdClientHandle(text); err != nil {
			t.Fatal(text, err)
		}
	}
	for _, text := range []string{"", "0x00000000", "1", "0x1", "0X00000001", "0x0000000g", "0x100000000", "0x00000001\n", "0x+0000001"} {
		if _, err := pdClientHandle(text); err == nil {
			t.Fatal("invalid handle accepted", text)
		}
	}
}

func fakePDProgram(t *testing.T, name, body string) string {
	t.Helper()
	file := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(file, []byte("#!/bin/sh\n"+body+"\n"), 0700); err != nil {
		t.Fatal(err)
	}
	return file
}
func TestPDUnkeyReadinessAndLifecycle(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("unprivileged launcher fixture required")
	}
	for _, bad := range []string{"echo '{\"event\":\"ready\",\"keys_transmitter\":true}'", "echo '{\"event\":\"other\",\"keys_transmitter\":false}'", "echo '{\"event\":\"ready\"}'", "echo invalid"} {
		r, w, _ := os.Pipe()
		api, _ := os.Open(os.DevNull)
		g, err := startAetherPDUnkey(fakePDProgram(t, "bad-guard", bad), api, r, 1, 2)
		r.Close()
		w.Close()
		api.Close()
		if err == nil || g != nil {
			t.Fatal("invalid guardian ready accepted", bad)
		}
	}
	api, err := os.Open(os.DevNull)
	if err != nil {
		t.Fatal(err)
	}
	defer api.Close()
	guardRead, guardWrite, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer guardRead.Close()
	defer guardWrite.Close()
	guard, err := startAetherPDUnkey(fakePDProgram(t, "guard", "echo '{\"event\":\"ready\",\"keys_transmitter\":false}'\ncat <&4 >/dev/null"), api, guardRead, 1, 2)
	if err != nil {
		t.Fatal(err)
	}
	iqRead, iqWrite, _ := os.Pipe()
	defer iqRead.Close()
	defer iqWrite.Close()
	setupRead, setupWrite, _ := os.Pipe()
	defer setupRead.Close()
	p, err := launchAetherPD(fakePDProgram(t, "dsp", "cat <&6 >/dev/null"), []*os.File{iqRead, api, guardWrite}, setupWrite, false, guard)
	if err != nil {
		t.Fatal(err)
	}
	// Parent copies must close after handoff, just as startInheritedAetherPD does.
	guardWrite.Close()
	guardRead.Close()
	select {
	case <-p.done:
		t.Fatal("DSP stopped before operator stop")
	case <-time.After(30 * time.Millisecond):
	}
	p.Stop()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err = p.Wait(ctx); err != nil {
		t.Fatal(err)
	}
	select {
	case <-guard.done:
	default:
		t.Fatal("normal shutdown did not reap guardian")
	}
}

func TestPDUnkeyFailureStopsOwnedDSP(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("unprivileged launcher fixture required")
	}
	api, trigger, _ := os.Pipe()
	defer api.Close()
	defer trigger.Close()
	guardRead, guardWrite, _ := os.Pipe()
	defer guardRead.Close()
	defer guardWrite.Close()
	// A helper can be ready and then fail. That must close this DSP's owner
	// pipe rather than allowing managed processing to continue without it.
	g, err := startAetherPDUnkey(fakePDProgram(t, "guard", "echo '{\"event\":\"ready\",\"keys_transmitter\":false}'\ncat <&3 >/dev/null\nexit 7"), api, guardRead, 1, 2)
	if err != nil {
		t.Fatal(err)
	}
	r, w, _ := os.Pipe()
	defer r.Close()
	defer w.Close()
	sr, sw, _ := os.Pipe()
	defer sr.Close()
	p, err := launchAetherPD(fakePDProgram(t, "dsp", "cat <&6 >/dev/null"), []*os.File{r, api, guardWrite}, sw, false, g)
	if err != nil {
		t.Fatal(err)
	}
	guardWrite.Close()
	trigger.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err = p.Wait(ctx); err == nil || !strings.Contains(err.Error(), "exit status 7") {
		t.Fatal("guardian failure not propagated", err)
	}
}

func TestPDGuardianPipeIdentity(t *testing.T) {
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	defer w.Close()
	otherR, otherW, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer otherR.Close()
	defer otherW.Close()
	if !samePDLifetimePipe(int(w.Fd()), int(r.Fd())) {
		t.Fatal("matching pipe rejected")
	}
	if samePDLifetimePipe(int(w.Fd()), int(otherR.Fd())) || samePDLifetimePipe(int(r.Fd()), int(w.Fd())) || samePDLifetimePipe(int(w.Fd()), int(w.Fd())) {
		t.Fatal("unrelated or reversed pipe accepted")
	}
}
