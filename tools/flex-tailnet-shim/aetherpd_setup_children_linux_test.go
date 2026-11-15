//go:build linux

package main

import (
	"context"
	"errors"
	"golang.org/x/sys/unix"
	"io"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

func TestPDSetupChildFixture(t *testing.T) {
	if os.Getenv("AETHER_PD_SETUP_CHILD_FIXTURE") != "1" {
		return
	}
	fds := []unix.PollFd{{Fd: 3}, {Fd: 4}}
	for {
		_, err := unix.Poll(fds, 100)
		if err == unix.EINTR {
			continue
		}
		if err != nil {
			os.Exit(8)
		}
		for _, fd := range fds {
			if fd.Revents&(unix.POLLHUP|unix.POLLERR|unix.POLLNVAL) != 0 {
				os.Exit(0)
			}
		}
	}
}
func setupChildrenFixture(t *testing.T) (pdSetupChildSpec, *os.File, []*os.File) {
	t.Helper()
	read, write, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer read.Close()
	spec := pdSetupChildSpec{present: true}
	triggers := make([]*os.File, 3)
	for i := range spec.pids {
		trigger, parent, err := os.Pipe()
		if err != nil {
			t.Fatal(err)
		}
		cmd := exec.Command(os.Args[0], "-test.run=^TestPDSetupChildFixture$")
		cmd.Env = []string{"AETHER_PD_SETUP_CHILD_FIXTURE=1", "GORACE=atexit_sleep_ms=0"}
		cmd.ExtraFiles = []*os.File{read, trigger}
		if err := cmd.Start(); err != nil {
			t.Fatal(err)
		}
		trigger.Close()
		spec.pids[i] = cmd.Process.Pid
		triggers[i] = parent
		t.Cleanup(func() { cmd.Process.Release() })
	}
	t.Cleanup(func() {
		write.Close()
		for _, trigger := range triggers {
			trigger.Close()
		}
	})
	return spec, write, triggers
}
func TestPDSetupChildrenMetadata(t *testing.T) {
	values := map[string]string{}
	get := func(key string) string { return values[key] }
	if spec, err := parsePDSetupChildren(get, 99); err != nil || spec.present {
		t.Fatal(spec, err)
	}
	values[pdSetupChildKeys[0]] = "12"
	if _, err := parsePDSetupChildren(get, 99); err == nil {
		t.Fatal("partial metadata accepted")
	}
	for i, key := range pdSetupChildKeys {
		values[key] = strconv.Itoa(100000 + i)
	}
	if _, err := parsePDSetupChildren(get, 99); err != nil {
		t.Fatal(err)
	}
	for _, bad := range []string{"0", "1", "-1", "+12", " 12", "12 ", "01", "12x", "2147483648", strconv.Itoa(os.Getpid()), "99", "100001"} {
		values[pdSetupChildKeys[0]] = bad
		if _, err := parsePDSetupChildren(get, 99); err == nil {
			t.Fatalf("bad PID accepted: %q", bad)
		}
	}
}
func TestPDSetupChildrenOrderlyRetirement(t *testing.T) {
	spec, write, _ := setupChildrenFixture(t)
	group, err := claimPDSetupChildren(spec)
	if err != nil {
		t.Fatal(err)
	}
	group.expectStop()
	write.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := group.wait(ctx); err != nil {
		t.Fatal(err)
	}
	select {
	case <-group.failed:
		t.Fatal("normal retirement classified as failure")
	default:
	}
	for _, pid := range spec.pids {
		if _, err := syscall.Wait4(pid, nil, syscall.WNOHANG, nil); !errors.Is(err, syscall.ECHILD) {
			t.Fatal("helper not reaped", pid, err)
		}
	}
}
func TestPDSetupChildExitStopsDSPAndReapsOthers(t *testing.T) {
	for failedRole := range pdSetupChildKeys {
		t.Run(pdSetupChildRoles[failedRole], func(t *testing.T) {
			spec, setup, trigger := setupChildrenFixture(t)
			group, err := claimPDSetupChildren(spec)
			if err != nil {
				t.Fatal(err)
			}
			ownerRead, ownerWrite, err := os.Pipe()
			if err != nil {
				t.Fatal(err)
			}
			defer ownerRead.Close()
			defer ownerWrite.Close()
			status, _ := privateStatusFixture(t)
			cal, _ := privateStatusFixture(t)
			guardDone := make(chan struct{})
			guard := &aetherPDUnkey{done: guardDone}
			wait := func() error { _, err := io.Copy(io.Discard, ownerRead); close(guardDone); return err }
			p := monitorAetherPDProcessWithSetup(0, wait, ownerWrite, status.status, cal.status, setup, guard, pdReceiveOriginal{}, nil, group)
			defer p.Stop()
			trigger[failedRole].Close()
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			err = p.Wait(ctx)
			if err == nil || !strings.Contains(err.Error(), pdSetupChildRoles[failedRole]) || ctx.Err() != nil {
				t.Fatal("helper loss did not stop/reap the session", err)
			}
			for _, pid := range spec.pids {
				if _, err := syscall.Wait4(pid, nil, syscall.WNOHANG, nil); !errors.Is(err, syscall.ECHILD) {
					t.Fatal("helper not reaped", pid, err)
				}
			}
		})
	}
}
func TestPDSetupChildAdmissionFailureReapsAdmittedSiblings(t *testing.T) {
	spec, write, triggers := setupChildrenFixture(t)
	unclaimed := spec.pids[1]
	spec.pids[1] = os.Getppid()
	group, err := claimPDSetupChildren(spec)
	if err == nil || group == nil {
		t.Fatal("non-child accepted", err)
	}
	group.expectStop()
	write.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := group.wait(ctx); err != nil {
		t.Fatal(err)
	}
	triggers[1].Close()
	if err := waitPDSetupChild(unclaimed); err != nil {
		t.Fatal(err)
	}
}

func TestPDSetupFailureBlocksPreparedRelease(t *testing.T) {
	f := newPreparedKernelFixture(t, 0)
	child := f.claim(t)
	group := &pdSetupChildren{failed: make(chan struct{}), done: make(chan struct{})}
	close(group.failed)
	guard := &aetherPDUnkey{done: make(chan struct{})}
	lease := &aetherPDLeaseGuard{done: make(chan struct{}), receive: &aetherPDLeaseGuard{done: make(chan struct{})}}
	original, _ := receiveRestoreFixture()
	setupR, setupW, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer setupR.Close()
	defer setupW.Close()
	if p, err := attachPreparedAetherPD(child, setupW, guard, original, lease, group); err == nil || p != nil {
		t.Fatal("failed setup released DSP", err)
	}
	buffer := make([]byte, 9)
	if _, _, err := syscall.Recvfrom(f.peers[2], buffer, syscall.MSG_DONTWAIT); err != syscall.EAGAIN {
		t.Fatal("failed setup sent gate release", err)
	}
}
