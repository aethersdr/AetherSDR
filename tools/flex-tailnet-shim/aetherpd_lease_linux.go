//go:build linux

package main

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"syscall"
	"time"
)

type aetherPDLeaseGuard struct {
	cmd     *exec.Cmd
	done    chan struct{}
	err     error
	receive *aetherPDLeaseGuard
}

type pdGuardianResult struct {
	retired bool // cmd.Wait joined the helper; its operation can still have failed.
	err     error
}

func (g *aetherPDLeaseGuard) waitResult(ctx context.Context) pdGuardianResult {
	select {
	case <-g.done:
		return pdGuardianResult{retired: true, err: g.err}
	case <-ctx.Done():
		return pdGuardianResult{err: ctx.Err()}
	}
}

func (g *aetherPDLeaseGuard) wait(ctx context.Context) error {
	return g.waitResult(ctx).err
}

func inheritedPDLeaseOwnership(value string) (bool, error) {
	switch value {
	case "", "0":
		return false, nil
	case "1":
		return true, nil
	default:
		return false, errors.New("SHIM_PD_OWN_LEASE_GUARD must be 0 or 1")
	}
}

func pdLeaseReadyFrame(slice int) string {
	return fmt.Sprintf("{\"event\":\"armed\",\"slice\":%d,\"protocol_errors\":0,\"stop_requested\":false,\"rf_commands\":[]}\n", slice)
}

// The API lease is still unregistered here: the caller captures the ordinary
// receive baseline, arms this helper, and only then starts the DSP. No API
// bytes are read by the helper until every lifetime writer has disappeared.
func startAetherPDLeaseGuard(program string, api, life *os.File, original pdReceiveOriginal) (*aetherPDLeaseGuard, error) {
	if api == nil || life == nil || !original.complete || original.guiHandle == 0 || original.panID == 0 || original.sliceID < 0 || original.sliceID >= 8 || (original.settings.mode != "USB" && original.settings.mode != "LSB") {
		return nil, errors.New("invalid prepared API lease guardian identity")
	}
	st, err := os.Stat(program)
	if err != nil {
		return nil, err
	}
	if !st.Mode().IsRegular() || st.Mode()&(os.ModeSetuid|os.ModeSetgid) != 0 {
		return nil, errors.New("invalid API lease guardian executable")
	}
	cmd := exec.Command(program, "3", "4", strconv.Itoa(original.sliceID), original.settings.mode, fmt.Sprintf("0x%08x", original.guiHandle), fmt.Sprintf("0x%08x", original.panID))
	cmd.Env = []string{"PATH=/usr/bin:/bin"}
	cmd.ExtraFiles = []*os.File{api, life}
	if os.Geteuid() == 0 {
		cmd.SysProcAttr = &syscall.SysProcAttr{Credential: &syscall.Credential{Uid: 78666, Gid: 20, Groups: []uint32{}}}
	}
	return startPDReadyCommand(cmd, pdLeaseReadyFrame(original.sliceID))
}

// Retain a bounded failure explanation while consuming all helper output.
// exec.Cmd.Wait joins the stderr copier before the diagnostic is inspected.
type pdGuardianDiagnostics struct {
	data []byte
}

func (d *pdGuardianDiagnostics) Write(data []byte) (int, error) {
	remaining := 1024 - len(d.data)
	if remaining > len(data) {
		remaining = len(data)
	}
	d.data = append(d.data, data[:remaining]...)
	return len(data), nil
}

func startPDReadyCommand(cmd *exec.Cmd, readyFrame string) (*aetherPDLeaseGuard, error) {
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	diagnostics := &pdGuardianDiagnostics{data: make([]byte, 0, 1024)}
	cmd.Stderr = diagnostics
	if err = cmd.Start(); err != nil {
		stdout.Close()
		return nil, err
	}
	g := &aetherPDLeaseGuard{cmd: cmd, done: make(chan struct{})}
	ready := make(chan error, 1)
	go func() {
		reader := bufio.NewReaderSize(stdout, 256)
		line, readErr := reader.ReadSlice('\n')
		if readErr == nil && string(line) != readyFrame {
			readErr = errors.New("session guardian readiness mismatch")
		}
		ready <- readErr
		// Only readiness goes to stdout; closing it cannot block cleanup.
		// Stderr is bounded and drained separately through cmd.Wait.
		stdout.Close()
		if readErr != nil {
			_ = cmd.Process.Kill()
		}
		waitErr := cmd.Wait()
		var diagnosticErr error
		if (readErr != nil || waitErr != nil) && len(diagnostics.data) > 0 {
			diagnosticErr = fmt.Errorf("session guardian diagnostics: %q", diagnostics.data)
		}
		g.err = errors.Join(readErr, waitErr, diagnosticErr)
		close(g.done)
	}()
	timer := time.NewTimer(3 * time.Second)
	defer timer.Stop()
	select {
	case err = <-ready:
		if err == nil {
			return g, nil
		}
	case <-timer.C:
		err = errors.New("session guardian readiness timeout")
	}
	// Startup precedes DSP registration, so this cannot release a registered
	// waveform's lease. Once ready, only lifetime closure controls this helper.
	_ = cmd.Process.Kill()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	_ = g.wait(ctx)
	return nil, err
}
