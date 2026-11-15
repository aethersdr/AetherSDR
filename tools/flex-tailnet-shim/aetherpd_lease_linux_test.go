//go:build linux

package main

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"
)

func TestPDLeaseOwnershipRequiresPreparedHandoff(t *testing.T) {
	for _, value := range []string{"", "0", "1"} {
		if _, err := inheritedPDLeaseOwnership(value); err != nil {
			t.Fatal(value, err)
		}
	}
	for _, value := range []string{"true", " 1", "01", "1\n"} {
		if _, err := inheritedPDLeaseOwnership(value); err == nil {
			t.Fatal("invalid ownership flag accepted", value)
		}
	}
	for _, key := range []string{"SHIM_PD_IQ_FD", "SHIM_PD_API_FD", "SHIM_PD_GUARD_FD", "SHIM_PD_SETUP_FD", "SHIM_PD_UNKEY_API_FD", "SHIM_PD_GUARD_READ_FD", "SHIM_PD_UNKEY_CLIENT_HANDLE", "SHIM_PD_GUI_HANDLE", "SHIM_PD_TRANSPORT_CACHE"} {
		t.Setenv(key, "")
	}
	t.Setenv("SHIM_PD_OWN_LEASE_GUARD", "1")
	if p, err := startInheritedAetherPD(); err == nil || p != nil {
		t.Fatal("lease ownership without prepared descriptors accepted")
	}
}

func TestPDLeaseReadinessAndLifetime(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("unprivileged helper fixture required")
	}
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	defer w.Close()
	api, err := os.Open(os.DevNull)
	if err != nil {
		t.Fatal(err)
	}
	defer api.Close()
	request, _, _ := receiveRecoveryFixture()
	for _, bad := range []string{"{}", "{\"event\":\"armed\",\"slice\":1}", "{\"event\":\"armed\",\"slice\":0,\"protocol_errors\":0,\"stop_requested\":false,\"rf_commands\":[\"xmit 1\"]}", "invalid"} {
		g, err := startAetherPDLeaseGuard(fakePDProgram(t, "lease-bad", "printf '%s\\n' '"+bad+"'"), api, r, request.original)
		if err == nil || g != nil {
			t.Fatal("invalid readiness accepted", bad)
		}
	}
	body := "trap '' PIPE\n[ \"$1 $2 $3 $4 $5 $6\" = \"3 4 0 USB 0x11223344 0x40000000\" ] || exit 4\nprintf '%s' '" + pdLeaseReadyFrame(0) + "'\ncat <&4 >/dev/null\necho complete\nexit 0"
	g, err := startAetherPDLeaseGuard(fakePDProgram(t, "lease", body), api, r, request.original)
	if err != nil {
		t.Fatal(err)
	}
	select {
	case <-g.done:
		t.Fatal("guardian ended before lifetime closed", g.err)
	case <-time.After(20 * time.Millisecond):
	}
	w.Close()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err = g.wait(ctx); err != nil {
		t.Fatal(err)
	}
}

func TestPDLeaseFailureStopsOwnedDSP(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("unprivileged helper fixture required")
	}
	api, trigger, _ := os.Pipe()
	defer api.Close()
	defer trigger.Close()
	lifeRead, lifeWrite, _ := os.Pipe()
	defer lifeRead.Close()
	defer lifeWrite.Close()
	request, _, _ := receiveRecoveryFixture()
	lease, err := startAetherPDLeaseGuard(fakePDProgram(t, "lease", "printf '%s' '"+pdLeaseReadyFrame(0)+"'\ncat <&3 >/dev/null\nexit 7"), api, lifeRead, request.original)
	if err != nil {
		t.Fatal(err)
	}
	null, _ := os.Open(os.DevNull)
	defer null.Close()
	unkey, err := startAetherPDUnkey(fakePDProgram(t, "unkey", "echo '{\"event\":\"ready\",\"keys_transmitter\":false}'\ncat <&4 >/dev/null"), null, lifeRead, 1, 2)
	if err != nil {
		t.Fatal(err)
	}
	iqRead, iqWrite, _ := os.Pipe()
	defer iqRead.Close()
	defer iqWrite.Close()
	setupRead, setupWrite, _ := os.Pipe()
	defer setupRead.Close()
	p, err := launchAetherPDWithSession(fakePDProgram(t, "dsp", "cat <&6 >/dev/null"), []*os.File{iqRead, null, lifeWrite}, setupWrite, false, unkey, pdReceiveOriginal{}, lease)
	if err != nil {
		t.Fatal(err)
	}
	lifeWrite.Close()
	trigger.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err = p.Wait(ctx); err == nil || ctx.Err() != nil {
		t.Fatal("helper failure did not terminate this owned DSP", err)
	}
	select {
	case <-unkey.done:
	default:
		t.Fatal("unkey helper not reaped")
	}
}

func TestPDReceiveGuardianFailureStopsOwnedDSP(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("unprivileged helper fixture required")
	}
	api, trigger, _ := os.Pipe()
	defer api.Close()
	defer trigger.Close()
	lifeRead, lifeWrite, _ := os.Pipe()
	defer lifeRead.Close()
	defer lifeWrite.Close()
	request, _, _ := receiveRecoveryFixture()
	lease, err := startAetherPDLeaseGuard(fakePDProgram(t, "lease", "printf '%s' '"+pdLeaseReadyFrame(0)+"'\ncat <&3 >/dev/null\nexit 7"), api, lifeRead, request.original)
	if err != nil {
		t.Fatal(err)
	}
	failedReceive := lease
	lease, err = startAetherPDLeaseGuard(fakePDProgram(t, "retained-lease", "printf '%s' '"+pdLeaseReadyFrame(0)+"'\ncat <&4 >/dev/null\nexit 0"), api, lifeRead, request.original)
	if err != nil {
		t.Fatal(err)
	}
	lease.receive = failedReceive
	null, _ := os.Open(os.DevNull)
	defer null.Close()
	unkey, err := startAetherPDUnkey(fakePDProgram(t, "unkey", "echo '{\"event\":\"ready\",\"keys_transmitter\":false}'\ncat <&4 >/dev/null"), null, lifeRead, 1, 2)
	if err != nil {
		t.Fatal(err)
	}
	iqRead, iqWrite, _ := os.Pipe()
	defer iqRead.Close()
	defer iqWrite.Close()
	setupRead, setupWrite, _ := os.Pipe()
	defer setupRead.Close()
	p, err := launchAetherPDWithSession(fakePDProgram(t, "dsp", "cat <&6 >/dev/null"), []*os.File{iqRead, null, lifeWrite}, setupWrite, false, unkey, pdReceiveOriginal{}, lease)
	if err != nil {
		t.Fatal(err)
	}
	lifeWrite.Close()
	trigger.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err = p.Wait(ctx); err == nil || ctx.Err() != nil {
		t.Fatal("helper failure did not terminate this owned DSP", err)
	}
	select {
	case <-unkey.done:
	default:
		t.Fatal("unkey helper not reaped")
	}
}

func TestPDReadyCommandDiagnosticsHelper(t *testing.T) {
	if os.Getenv("AETHER_PD_TEST_GUARDIAN_DIAGNOSTICS") != "1" {
		return
	}
	os.Stdout.WriteString(pdReceiveGuardianReady)
	os.Stderr.WriteString("AetherPD: receive guardian refused restoration: original DAX channel is occupied\n")
	for range 128 {
		os.Stderr.WriteString(strings.Repeat("x", 4096))
	}
	os.Exit(4)
}

func TestPDReadyCommandRetainsBoundedFailureAndDrainsStderr(t *testing.T) {
	cmd := exec.Command(os.Args[0], "-test.run=^TestPDReadyCommandDiagnosticsHelper$")
	cmd.Env = []string{"AETHER_PD_TEST_GUARDIAN_DIAGNOSTICS=1"}
	guardian, err := startPDReadyCommand(cmd, pdReceiveGuardianReady)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	result := guardian.waitResult(ctx)
	err = result.err
	if !result.retired {
		t.Fatal("exited receive helper was not observed reaped")
	}
	if err == nil || !strings.Contains(err.Error(), "receive guardian refused restoration: original DAX channel is occupied") || len(err.Error()) > 1400 {
		t.Fatalf("missing or unbounded guardian failure: %v", err)
	}
	if ctx.Err() != nil {
		t.Fatal("stderr backpressure blocked guardian retirement")
	}
}

func TestPDGuardianWaitResultRequiresRetirement(t *testing.T) {
	guardian := &aetherPDLeaseGuard{done: make(chan struct{})}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	result := guardian.waitResult(ctx)
	if result.retired || !errors.Is(result.err, context.Canceled) {
		t.Fatal("canceled wait fabricated worker retirement", result)
	}
	guardian.err = errors.New("receive recovery refused")
	close(guardian.done)
	result = guardian.waitResult(context.Background())
	if !result.retired || result.err != guardian.err {
		t.Fatal("worker exit discarded recovery error", result)
	}
}
