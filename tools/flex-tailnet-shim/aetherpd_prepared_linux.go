//go:build linux

package main

import (
	"errors"
	"fmt"
	"net"
	"os"
	"os/exec"
	"strconv"
	"syscall"
	"time"
)

var pdPreparedKeys = [...]string{"SHIM_PD_DSP_PID", "SHIM_PD_OWNER_FD", "SHIM_PD_STATUS_FD", "SHIM_PD_CALIBRATION_FD", "SHIM_PD_START_FD"}

type pdPreparedSpec struct {
	present bool
	pid     int
	fds     [4]int
}

func parsePDPrepared(get func(string) string) (pdPreparedSpec, error) {
	var s pdPreparedSpec
	count := 0
	for _, key := range pdPreparedKeys {
		if get(key) != "" {
			count++
		}
	}
	if count == 0 {
		return s, nil
	}
	if count != len(pdPreparedKeys) {
		return s, errors.New("incomplete prepared DSP handoff")
	}
	text := get(pdPreparedKeys[0])
	if len(text) == 0 || len(text) > 10 || text[0] == '0' {
		return s, errors.New("invalid prepared DSP child")
	}
	for _, c := range text {
		if c < '0' || c > '9' {
			return s, errors.New("invalid prepared DSP child")
		}
	}
	pid, err := strconv.ParseInt(text, 10, 32)
	if err != nil || pid < 2 || int(pid) == os.Getpid() {
		return s, errors.New("invalid prepared DSP child")
	}
	s.present = true
	s.pid = int(pid)
	for i, key := range pdPreparedKeys[1:] {
		value := get(key)
		if len(value) == 0 || len(value) > 4 {
			return pdPreparedSpec{}, errors.New("invalid prepared DSP descriptor")
		}
		for _, c := range value {
			if c < '0' || c > '9' {
				return pdPreparedSpec{}, errors.New("invalid prepared DSP descriptor")
			}
		}
		fd, err := strconv.Atoi(value)
		if err != nil || fd < 3 || fd > 1023 {
			return pdPreparedSpec{}, errors.New("invalid prepared DSP descriptor")
		}
		s.fds[i] = fd
	}
	return s, nil
}

type pdPreparedChild struct {
	process             *os.Process
	owner               *os.File
	status, calibration *net.UnixConn
	gate                *os.File
	monitored           bool
}

func validatePDPreparedDescriptors(s pdPreparedSpec, existing []int) error {
	if !s.present {
		return errors.New("prepared DSP handoff absent")
	}
	all := append(append([]int{}, existing...), s.fds[:]...)
	// The original session includes two expected ends of one pipe. Every new
	// endpoint must be distinct from those and every other new role.
	for i, fd := range s.fds {
		var st syscall.Stat_t
		if err := syscall.Fstat(fd, &st); err != nil {
			return err
		}
		for j, other := range all[:len(existing)+i] {
			var prior syscall.Stat_t
			if fd == other || syscall.Fstat(other, &prior) != nil || st.Dev == prior.Dev && st.Ino == prior.Ino {
				return fmt.Errorf("prepared DSP descriptor aliases role%d", j)
			}
		}
		if i == 0 {
			flags, _, e := syscall.Syscall(syscall.SYS_FCNTL, uintptr(fd), syscall.F_GETFL, 0)
			if e != 0 || flags&syscall.O_ACCMODE != syscall.O_WRONLY || st.Mode&syscall.S_IFMT != syscall.S_IFIFO {
				return errors.New("invalid prepared DSP owner pipe")
			}
		} else {
			kind, err := syscall.GetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_TYPE)
			if err != nil || kind != syscall.SOCK_DGRAM || !privatePDUnixEndpoint(fd, false) || !privatePDUnixEndpoint(fd, true) {
				return errors.New("invalid prepared DSP private endpoint")
			}
		}
	}
	return nil
}
func preparedPDUnix(file *os.File) (*net.UnixConn, error) {
	defer file.Close()
	conn, err := net.FileConn(file)
	if err != nil {
		return nil, err
	}
	value, ok := conn.(*net.UnixConn)
	if !ok {
		conn.Close()
		return nil, errors.New("invalid prepared DSP connection")
	}
	return value, nil
}
func claimPDPreparedChild(s pdPreparedSpec, existing []int) (*pdPreparedChild, error) {
	if err := validatePDPreparedDescriptors(s, existing); err != nil {
		return nil, err
	}
	for _, fd := range s.fds {
		syscall.CloseOnExec(fd)
	}
	// wait4 admits only a direct child. No signal or scheduling operation is
	// performed on this number, including when admission fails.
	var status syscall.WaitStatus
	got, err := syscall.Wait4(s.pid, &status, syscall.WNOHANG, nil)
	if err != nil || got != 0 {
		return nil, errors.New("prepared DSP is not a live direct child")
	}
	process, err := os.FindProcess(s.pid)
	if err != nil {
		return nil, err
	}
	child := &pdPreparedChild{process: process, owner: os.NewFile(uintptr(s.fds[0]), "prepared-DSP-owner"), gate: os.NewFile(uintptr(s.fds[3]), "prepared-DSP-start")}
	statusFile := os.NewFile(uintptr(s.fds[1]), "prepared-DSP-status")
	calibrationFile := os.NewFile(uintptr(s.fds[2]), "prepared-DSP-calibration")
	defer statusFile.Close()
	defer calibrationFile.Close()
	child.status, err = preparedPDUnix(statusFile)
	if err == nil {
		child.calibration, err = preparedPDUnix(calibrationFile)
	}
	if err != nil {
		cleanupErr := child.close()
		return nil, errors.Join(err, cleanupErr)
	}
	return child, nil
}
func (c *pdPreparedChild) close() error {
	if c == nil {
		return nil
	}
	if c.owner != nil {
		c.owner.Close()
	}
	if c.status != nil {
		c.status.Close()
	}
	if c.calibration != nil {
		c.calibration.Close()
	}
	if c.gate != nil {
		c.gate.Close()
	}
	if c.process != nil && !c.monitored {
		process := c.process
		c.process = nil
		reaped := make(chan error, 1)
		go func() { reaped <- waitPDPreparedChild(process) }()
		select {
		case err := <-reaped:
			return err
		case <-time.After(5 * time.Second):
			return errors.New("prepared DSP did not exit after startup owner closure")
		}
	}
	return nil
}
func waitPDPreparedChild(process *os.Process) error {
	state, err := process.Wait()
	if err != nil {
		return err
	}
	if !state.Success() {
		return &exec.ExitError{ProcessState: state}
	}
	return nil
}
func attachPreparedAetherPD(child *pdPreparedChild, setup *os.File, guard *aetherPDUnkey, original pdReceiveOriginal, lease *aetherPDLeaseGuard, groups ...*pdSetupChildren) (*aetherPDProcess, error) {
	var setupChildren *pdSetupChildren
	if len(groups) > 1 {
		return nil, errors.New("invalid setup supervision")
	}
	if len(groups) == 1 {
		setupChildren = groups[0]
	}
	if err := setupChildren.validate(); err != nil {
		return nil, err
	}
	if child == nil || child.process == nil || setup == nil || !original.complete || original.settings.mode != "USB" || lease == nil || lease.receive == nil {
		return nil, errors.New("incomplete prepared DSP supervision")
	}
	if err := validateAetherPDGuardians(guard, lease); err != nil {
		return nil, err
	}
	p := monitorAetherPDProcessWithSetup(child.process.Pid, func() error { return waitPDPreparedChild(child.process) }, child.owner, child.status, child.calibration, setup, guard, original, lease, setupChildren)
	child.monitored = true
	// The native gate checks owner closure before accepting release. Starting
	// monitoring first makes guardian death close that same owner pipe.
	if err := errors.Join(validateAetherPDGuardians(guard, lease), setupChildren.validate()); err != nil {
		p.Stop()
		child.gate.Close()
		return nil, err
	}
	sent, err := syscall.SendmsgN(int(child.gate.Fd()), []byte("PDRUN001"), nil, nil, syscall.MSG_DONTWAIT|syscall.MSG_NOSIGNAL)
	child.gate.Close()
	if err != nil || sent != 8 {
		p.Stop()
		return nil, errors.New("prepared DSP release failed")
	}
	return p, nil
}
