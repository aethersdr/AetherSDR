//go:build linux

package main

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"strconv"
	"syscall"
	"testing"
	"time"
)

func preparedEnvironment() map[string]string {
	return map[string]string{"SHIM_PD_DSP_PID": "2147483647", "SHIM_PD_OWNER_FD": "30", "SHIM_PD_STATUS_FD": "31", "SHIM_PD_CALIBRATION_FD": "32", "SHIM_PD_START_FD": "33"}
}
func TestPDPreparedMetadata(t *testing.T) {
	get := func(string) string { return "" }
	if s, e := parsePDPrepared(get); e != nil || s.present {
		t.Fatal(s, e)
	}
	for _, key := range pdPreparedKeys {
		v := preparedEnvironment()
		delete(v, key)
		if _, e := parsePDPrepared(func(k string) string { return v[k] }); e == nil {
			t.Fatal("partial handoff accepted", key)
		}
	}
	for _, value := range []string{"0", "1", "-1", "+42", "042", "42 ", " 42", "2147483648", "4294967295", strconv.Itoa(os.Getpid())} {
		v := preparedEnvironment()
		v[pdPreparedKeys[0]] = value
		if _, e := parsePDPrepared(func(k string) string { return v[k] }); e == nil {
			t.Fatal("invalid child accepted", value)
		}
	}
	for _, key := range pdPreparedKeys[1:] {
		for _, value := range []string{"0", "2", "-3", "+3", "1024", "3x", "3 ", "00003"} {
			v := preparedEnvironment()
			v[key] = value
			if _, e := parsePDPrepared(func(k string) string { return v[k] }); e == nil {
				t.Fatal("invalid descriptor accepted", key, value)
			}
		}
	}
	v := preparedEnvironment()
	s, e := parsePDPrepared(func(k string) string { return v[k] })
	if e != nil || !s.present || s.pid != 2147483647 || s.fds != [4]int{30, 31, 32, 33} {
		t.Fatal(s, e)
	}
}

// Kernel pipes/socketpairs and a shell waiting for stdin closure; no listener,
// firmware simulator, radio connection, scheduler change or RF control.
type preparedKernelFixture struct {
	spec    pdPreparedSpec
	peers   [3]int
	parents [4]int
	command *exec.Cmd
	claimed bool
	child   *pdPreparedChild
}

func newPreparedKernelFixture(t *testing.T, code int) *preparedKernelFixture {
	t.Helper()
	f := &preparedKernelFixture{}
	var pipe [2]int
	if e := syscall.Pipe2(pipe[:], syscall.O_CLOEXEC); e != nil {
		t.Fatal(e)
	}
	input := os.NewFile(uintptr(pipe[0]), "prepared-test-child-stdin")
	f.command = exec.Command("/bin/sh", "-c", "read value; exit "+strconv.Itoa(code))
	f.command.Stdin = input
	if e := f.command.Start(); e != nil {
		input.Close()
		syscall.Close(pipe[1])
		t.Fatal(e)
	}
	input.Close()
	f.parents[0] = pipe[1]
	f.spec = pdPreparedSpec{present: true, pid: f.command.Process.Pid}
	f.spec.fds[0] = pipe[1]
	for i := 0; i < 3; i++ {
		p, e := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_DGRAM|syscall.SOCK_CLOEXEC, 0)
		if e != nil {
			t.Fatal(e)
		}
		f.parents[i+1] = p[0]
		f.peers[i] = p[1]
		f.spec.fds[i+1] = p[0]
	}
	t.Cleanup(func() {
		if f.claimed {
			if f.child != nil {
				f.child.close()
			}
			f.command.Process.Release()
		} else {
			for _, fd := range f.parents {
				syscall.Close(fd)
			}
			f.command.Wait()
		}
		for _, fd := range f.peers {
			syscall.Close(fd)
		}
	})
	return f
}
func (f *preparedKernelFixture) claim(t *testing.T) *pdPreparedChild {
	t.Helper()
	child, e := claimPDPreparedChild(f.spec, nil)
	if e != nil {
		t.Fatal(e)
	}
	f.claimed = true
	f.child = child
	return child
}
func TestPDPreparedChildOwnershipAndCleanup(t *testing.T) {
	t.Run("unrelated PID", func(t *testing.T) {
		f := newPreparedKernelFixture(t, 0)
		s := f.spec
		s.pid = os.Getppid()
		if child, e := claimPDPreparedChild(s, nil); e == nil || child != nil {
			t.Fatal("non-child accepted")
		}
		for _, fd := range f.parents {
			var st syscall.Stat_t
			if e := syscall.Fstat(fd, &st); e != nil {
				t.Fatal("rejected handoff closed a borrowed descriptor", e)
			}
		}
	})
	t.Run("existing role alias", func(t *testing.T) {
		f := newPreparedKernelFixture(t, 0)
		if e := validatePDPreparedDescriptors(f.spec, []int{f.spec.fds[1]}); e == nil {
			t.Fatal("existing endpoint alias accepted")
		}
	})
	t.Run("duplicate endpoint", func(t *testing.T) {
		f := newPreparedKernelFixture(t, 0)
		s := f.spec
		s.fds[2] = s.fds[1]
		if e := validatePDPreparedDescriptors(s, nil); e == nil {
			t.Fatal("duplicate endpoint accepted")
		}
	})
	t.Run("duplicate open description", func(t *testing.T) {
		f := newPreparedKernelFixture(t, 0)
		dup, e := syscall.Dup(f.spec.fds[1])
		if e != nil {
			t.Fatal(e)
		}
		defer syscall.Close(dup)
		s := f.spec
		s.fds[2] = dup
		if e := validatePDPreparedDescriptors(s, nil); e == nil {
			t.Fatal("duplicated endpoint accepted")
		}
	})
	t.Run("wrong owner access", func(t *testing.T) {
		f := newPreparedKernelFixture(t, 0)
		s := f.spec
		s.fds[0] = s.fds[1]
		if e := validatePDPreparedDescriptors(s, nil); e == nil {
			t.Fatal("non-pipe owner accepted")
		}
	})
	t.Run("startup closure reaps child", func(t *testing.T) {
		f := newPreparedKernelFixture(t, 0)
		child := f.claim(t)
		if e := child.close(); e != nil {
			t.Fatal(e)
		}
		var state syscall.WaitStatus
		if _, e := syscall.Wait4(f.spec.pid, &state, syscall.WNOHANG, nil); e != syscall.ECHILD {
			t.Fatal("child not reaped", e)
		}
	})
}
func TestPDPreparedExitClassification(t *testing.T) {
	for _, code := range []int{0, 7} {
		t.Run(strconv.Itoa(code), func(t *testing.T) {
			f := newPreparedKernelFixture(t, code)
			child := f.claim(t)
			child.owner.Close()
			child.monitored = true
			e := waitPDPreparedChild(child.process)
			if pdReceiveAPILeaseFailed(e) != (code == 7) {
				t.Fatal("wrong prepared-child classification", e)
			}
			if code == 0 && e != nil {
				t.Fatal(e)
			}
		})
	}
}
func TestPDPreparedGuardianAdmission(t *testing.T) {
	for _, ended := range []string{"unkey", "lease", "receive"} {
		t.Run(ended, func(t *testing.T) {
			f := newPreparedKernelFixture(t, 0)
			child := f.claim(t)
			guard := &aetherPDUnkey{done: make(chan struct{})}
			lease := &aetherPDLeaseGuard{done: make(chan struct{}), receive: &aetherPDLeaseGuard{done: make(chan struct{})}}
			switch ended {
			case "unkey":
				close(guard.done)
			case "lease":
				close(lease.done)
			case "receive":
				close(lease.receive.done)
			}
			original, _ := receiveRestoreFixture()
			setupR, setupW, e := os.Pipe()
			if e != nil {
				t.Fatal(e)
			}
			defer setupR.Close()
			defer setupW.Close()
			if p, e := attachPreparedAetherPD(child, setupW, guard, original, lease); e == nil || p != nil {
				t.Fatal("dead guardian admitted", p, e)
			}
			b := make([]byte, 9)
			if _, _, e := syscall.Recvfrom(f.peers[2], b, syscall.MSG_DONTWAIT); e != syscall.EAGAIN {
				t.Fatal("release reached child before guardians", e)
			}
		})
	}
}
func TestPDPreparedAttachReleaseAndStop(t *testing.T) {
	f := newPreparedKernelFixture(t, 7)
	child := f.claim(t)
	guard := &aetherPDUnkey{done: make(chan struct{})}
	lease := &aetherPDLeaseGuard{done: make(chan struct{}), receive: &aetherPDLeaseGuard{done: make(chan struct{})}}
	original, _ := receiveRestoreFixture()
	setupR, setupW, e := os.Pipe()
	if e != nil {
		t.Fatal(e)
	}
	defer setupR.Close()
	defer setupW.Close()
	p, e := attachPreparedAetherPD(child, setupW, guard, original, lease)
	if e != nil {
		t.Fatal(e)
	}
	b := make([]byte, 9)
	n, _, e := syscall.Recvfrom(f.peers[2], b, syscall.MSG_DONTWAIT)
	if e != nil || string(b[:n]) != "PDRUN001" {
		t.Fatal("wrong private release", n, e, b)
	}
	if !child.monitored || p.pid != f.spec.pid {
		t.Fatal("wrong child monitor")
	}
	p.Stop()
	close(guard.done)
	close(lease.done)
	close(lease.receive.done)
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	e = p.Wait(ctx)
	var exited *exec.ExitError
	if !errors.As(e, &exited) || !pdReceiveAPILeaseFailed(e) {
		t.Fatal("missing dedicated child exit", e)
	}
	var state syscall.WaitStatus
	if _, e := syscall.Wait4(f.spec.pid, &state, syscall.WNOHANG, nil); e != syscall.ECHILD {
		t.Fatal("monitor did not reap child", e)
	}
}

func TestPDPreparedRequiresCompleteSession(t *testing.T) {
	for _, key := range []string{"SHIM_PD_IQ_FD", "SHIM_PD_API_FD", "SHIM_PD_GUARD_FD", "SHIM_PD_SETUP_FD", "SHIM_PD_UNKEY_API_FD", "SHIM_PD_GUARD_READ_FD", "SHIM_PD_UNKEY_CLIENT_HANDLE", "SHIM_PD_GUI_HANDLE", "SHIM_PD_OWN_LEASE_GUARD", "SHIM_PD_TRANSPORT_CACHE"} {
		t.Setenv(key, "")
	}
	for key, value := range preparedEnvironment() {
		t.Setenv(key, value)
	}
	if p, e := startInheritedAetherPD(); p != nil || e == nil {
		t.Fatal("standalone shim accepted prepared child metadata", p, e)
	}
}
func TestPDPreparedReleaseFailureReapsChild(t *testing.T) {
	f := newPreparedKernelFixture(t, 0)
	child := f.claim(t)
	guard := &aetherPDUnkey{done: make(chan struct{})}
	lease := &aetherPDLeaseGuard{done: make(chan struct{}), receive: &aetherPDLeaseGuard{done: make(chan struct{})}}
	original, _ := receiveRestoreFixture()
	setupR, setupW, e := os.Pipe()
	if e != nil {
		t.Fatal(e)
	}
	defer setupR.Close()
	defer setupW.Close()
	syscall.Close(f.peers[2])
	f.peers[2] = -1
	if p, e := attachPreparedAetherPD(child, setupW, guard, original, lease); e == nil || p != nil {
		t.Fatal("dead release peer accepted", p, e)
	}
	close(guard.done)
	close(lease.done)
	close(lease.receive.done)
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		if _, e := os.Stat("/proc/" + strconv.Itoa(f.spec.pid)); os.IsNotExist(e) {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatal("release failure left direct child unreaped")
}
