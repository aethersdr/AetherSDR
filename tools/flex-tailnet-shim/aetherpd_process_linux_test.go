//go:build linux

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

func TestPDReceiveExitClassification(t *testing.T) {
	for _, test := range []struct {
		script    string
		apiFailed bool
	}{
		{"exit 0", false}, {"exit 1", false}, {"exit 7", true}, {"kill -KILL $$", false},
	} {
		err := exec.Command("/bin/sh", "-c", test.script).Run()
		if got := pdReceiveAPILeaseFailed(err); got != test.apiFailed {
			t.Fatalf("%s: API failure=%v, want %v", test.script, got, test.apiFailed)
		}
	}
}

func TestAetherPDPrivateSocketProof(t *testing.T) {
	pair, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_DGRAM, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer syscall.Close(pair[0])
	defer syscall.Close(pair[1])
	if !privatePDUnixEndpoint(pair[0], false) || !privatePDUnixEndpoint(pair[0], true) {
		t.Fatal("unnamed socketpair rejected")
	}
	named, err := syscall.Socket(syscall.AF_UNIX, syscall.SOCK_DGRAM, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer syscall.Close(named)
	if err = syscall.Bind(named, &syscall.SockaddrUnix{Name: "@pd-boundary-" + strconv.Itoa(os.Getpid())}); err != nil {
		t.Fatal(err)
	}
	if privatePDUnixEndpoint(named, false) {
		t.Fatal("named abstract endpoint accepted as private")
	}
}

func TestAetherPDInheritedConfig(t *testing.T) {
	t.Setenv("SHIM_PD_TRANSPORT_CACHE", "")
	for _, key := range []string{"SHIM_PD_IQ_FD", "SHIM_PD_API_FD", "SHIM_PD_GUARD_FD", "SHIM_PD_SETUP_FD", "SHIM_PD_UNKEY_API_FD", "SHIM_PD_GUARD_READ_FD", "SHIM_PD_UNKEY_CLIENT_HANDLE", "SHIM_PD_GUI_HANDLE"} {
		t.Setenv(key, "")
	}
	if p, err := startInheritedAetherPD(); p != nil || err != nil {
		t.Fatal("standalone shim changed", p, err)
	}
	t.Setenv("SHIM_PD_TRANSPORT_CACHE", "1")
	if _, err := startInheritedAetherPD(); err == nil {
		t.Fatal("recovery option without a session accepted")
	}
	t.Setenv("SHIM_PD_TRANSPORT_CACHE", "")
	t.Setenv("SHIM_PD_IQ_FD", "3")
	if _, err := startInheritedAetherPD(); err == nil {
		t.Fatal("partial setup accepted")
	}
	for _, key := range []string{"SHIM_PD_API_FD", "SHIM_PD_GUARD_FD", "SHIM_PD_SETUP_FD"} {
		t.Setenv(key, "3")
	}
	if _, err := startInheritedAetherPD(); err == nil {
		t.Fatal("aliased descriptors accepted")
	}
	for _, bad := range []string{"-1", "+3", "0", "1024", "3x"} {
		t.Setenv("SHIM_PD_IQ_FD", bad)
		if _, err := startInheritedAetherPD(); err == nil {
			t.Fatal("invalid descriptor accepted", bad)
		}
	}
}

func TestAetherPDStopOwnershipAndAuth(t *testing.T) {
	n := newTestNode(t, "secret")
	h := NewAPI(n)
	if got := call(t, h, "POST", "/v1/aetherpd/stop", "192.168.50.20:4000", "secret", ""); got.Code != http.StatusConflict {
		t.Fatal("unowned process can be stopped", got.Code)
	}
	read, write, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer read.Close()
	defer write.Close()
	n.pdProcess = &aetherPDProcess{owner: write}
	for _, tc := range []struct {
		token, path, body string
		code              int
	}{
		{"wrong", "/v1/aetherpd/stop", "", http.StatusUnauthorized},
		{"secret", "/v1/aetherpd/stop?pid=1", "", http.StatusBadRequest},
		{"secret", "/v1/aetherpd/stop", `{"command":"anything"}`, http.StatusBadRequest},
	} {
		if got := call(t, h, "POST", tc.path, "192.168.50.20:4000", tc.token, tc.body); got.Code != tc.code {
			t.Fatal(got.Code, tc)
		}
		if _, err := write.Write([]byte("x")); err != nil {
			t.Fatal("rejected request stopped process", err)
		}
	}
	for i := 0; i < 2; i++ {
		if got := call(t, h, "POST", "/v1/aetherpd/stop", "192.168.50.20:4000", "secret", ""); got.Code != http.StatusAccepted {
			t.Fatal(got.Code)
		}
	}
	if _, err := write.Write([]byte("x")); err == nil {
		t.Fatal("stop did not close lifetime writer")
	}
}

func TestAetherPDLiveProcess(t *testing.T) {
	mode := os.Getenv("AETHER_PD_LIVE_OWNER_MODE")
	if mode == "" {
		t.Skip("receive-only native lifecycle harness required")
	}
	if mode != "stop" && mode != "crash" {
		t.Fatal("invalid test mode")
	}
	process, err := startInheritedAetherPD()
	if err != nil || process == nil {
		t.Fatal("launch failed", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	if mode == "stop" {
		t.Cleanup(func() { process.Stop(); _ = process.Wait(ctx) })
	}
	for {
		select {
		case <-process.done:
			t.Fatal("DSP exited before registration", process.err)
		default:
		}
		status, err := process.queryStatus(ctx)
		if err == nil && status.Available && status.Registered && !status.Keyed {
			break
		}
		if ctx.Err() != nil {
			t.Fatal("native registration timeout", err)
		}
		time.Sleep(20 * time.Millisecond)
	}
	fmt.Printf("PD_CHILD_PID=%d\n", process.pid)
	// Exercise actual feedback delivery before either lifecycle termination.
	time.Sleep(2 * time.Second)
	data, err := process.readNativeStatus(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var feedback struct {
		Frames         uint64 `json:"local_iq_frames"`
		Stale          uint64 `json:"local_iq_stale"`
		Fault          bool   `json:"local_iq_fault"`
		Keyed          bool   `json:"keyed"`
		TransportCache bool   `json:"transport_cache_enabled"`
	}
	if err := json.Unmarshal(data, &feedback); err != nil {
		t.Fatal(err)
	}
	// Model/status initialization invalidates correction until calibration;
	// local_iq_fault is therefore not a transport-only health indicator.
	if feedback.Frames < 100 || feedback.Stale != 0 || feedback.Keyed {
		t.Fatal("receive-only feedback delivery failed", feedback)
	}
	if feedback.TransportCache != (os.Getenv("SHIM_PD_TRANSPORT_CACHE") == "1") {
		t.Fatal("managed recovery option mismatch", feedback)
	}
	fmt.Printf("PD_RECEIVED_IQ_FRAMES=%d STALE=%d CALIBRATION_REQUIRED=%t\n", feedback.Frames, feedback.Stale, feedback.Fault)
	if mode == "crash" {
		t.Log("killing only the temporary shim test process; DSP owner pipe must end without Pdeathsig")
		_ = syscall.Kill(os.Getpid(), syscall.SIGKILL)
		select {}
	}
	n := &Node{pdProcess: process}
	n.cfg.TokenSHA256 = sha256Hex("local-one-run-probe")
	if got := call(t, NewAPI(n), "GET", "/v1/aetherpd/status", "192.168.50.20:4000", "local-one-run-probe", ""); got.Code != http.StatusOK {
		t.Fatal(got.Code, got.Body.String())
	}
	reply := call(t, NewAPI(n), "POST", "/v1/aetherpd/stop", "192.168.50.20:4000", "local-one-run-probe", "")
	if reply.Code != http.StatusAccepted || !strings.Contains(reply.Body.String(), "stop_requested") {
		t.Fatal(reply.Code, reply.Body.String())
	}
	if err := process.Wait(ctx); err != nil {
		t.Fatal("DSP did not exit cleanly", err)
	}
	if got := call(t, NewAPI(n), "GET", "/v1/aetherpd/status", "192.168.50.20:4000", "local-one-run-probe", ""); got.Code != http.StatusServiceUnavailable {
		t.Fatal("dead session redirected status", got.Code)
	}
	t.Log("authenticated stop ended the real native DSP through its lifetime pipe")
}

func TestAetherPDTransportCacheOption(t *testing.T) {
	for _, value := range []string{"", "0", "1", "true", "-1", "1\n", "2"} {
		got, err := inheritedPDTransportCache(value)
		valid := value == "" || value == "0" || value == "1"
		if (err == nil) != valid || got != (value == "1") {
			t.Fatal(value, got, err)
		}
	}
}

func TestPDManagedLaunchRejectsUnsupportedModulation(t *testing.T) {
	request, _, _ := receiveRecoveryFixture()
	setup, err := os.Open(os.DevNull)
	if err != nil {
		t.Fatal(err)
	}
	defer setup.Close()
	for _, mode := range []string{"LSB", "DIGU", "DIGL", "CW", "AM", "SAM", "FM", "PD", ""} {
		original := request.original
		original.settings.mode = mode
		p, err := launchAetherPDWithSession("/never-executed", make([]*os.File, 3), setup, false, &aetherPDUnkey{}, original, nil)
		if p != nil || err == nil || !strings.Contains(err.Error(), "supports USB only") {
			t.Fatalf("mode %q not rejected before executable access: %v", mode, err)
		}
	}
}
