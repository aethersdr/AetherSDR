//go:build linux

package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
)

var pdSetupChildKeys = [...]string{"SHIM_PD_SOURCE_PID", "SHIM_PD_CAPTURE_PID", "SHIM_PD_ARP_PID"}
var pdSetupChildRoles = [...]string{"IQ source", "IQ capture", "ARP keeper"}

type pdSetupChildSpec struct {
	present bool
	pids    [3]int
}

func parsePDSetupChildren(get func(string) string, dsp int) (pdSetupChildSpec, error) {
	var spec pdSetupChildSpec
	count := 0
	for _, key := range pdSetupChildKeys {
		if get(key) != "" {
			count++
		}
	}
	if count == 0 {
		return spec, nil
	}
	if count != len(pdSetupChildKeys) {
		return spec, errors.New("incomplete setup child handoff")
	}
	spec.present = true
	for i, key := range pdSetupChildKeys {
		text := get(key)
		if len(text) == 0 || len(text) > 10 || text[0] == '0' {
			return pdSetupChildSpec{}, errors.New("invalid setup child PID")
		}
		for _, c := range text {
			if c < '0' || c > '9' {
				return pdSetupChildSpec{}, errors.New("invalid setup child PID")
			}
		}
		value, err := strconv.ParseInt(text, 10, 32)
		if err != nil || value < 2 || int(value) == os.Getpid() || int(value) == dsp {
			return pdSetupChildSpec{}, errors.New("invalid setup child PID")
		}
		for _, prior := range spec.pids[:i] {
			if prior == int(value) {
				return pdSetupChildSpec{}, errors.New("setup child roles must be distinct")
			}
		}
		spec.pids[i] = int(value)
	}
	return spec, nil
}

type pdSetupChildren struct {
	expected atomic.Bool
	failed   chan struct{}
	done     chan struct{}
	mu       sync.Mutex
	errs     []error
}

func claimPDSetupChildren(spec pdSetupChildSpec) (*pdSetupChildren, error) {
	if !spec.present {
		return nil, nil
	}
	for i, pid := range spec.pids {
		if pid < 2 || pid == os.Getpid() {
			return nil, errors.New("invalid setup child PID")
		}
		for _, prior := range spec.pids[:i] {
			if prior == pid {
				return nil, errors.New("duplicate setup child")
			}
		}
	}
	group := &pdSetupChildren{failed: make(chan struct{}), done: make(chan struct{})}
	var children [3]int
	var admission error
	// Validate every role even after a rejection so already-admitted children
	// still have waiters. No signal, scheduling verb or wait(-1) is used here.
	for i, pid := range spec.pids {
		var status syscall.WaitStatus
		got, err := syscall.Wait4(pid, &status, syscall.WNOHANG, nil)
		if err != nil || got != 0 {
			admission = errors.Join(admission, fmt.Errorf("%s is not a live direct child", pdSetupChildRoles[i]))
			continue
		}
		children[i] = pid
	}
	var waits sync.WaitGroup
	var failure sync.Once
	for i, child := range children {
		if child == 0 {
			continue
		}
		waits.Add(1)
		go func(role string, pid int) {
			defer waits.Done()
			err := waitPDSetupChild(pid)
			if !group.expected.Load() {
				err = errors.Join(fmt.Errorf("%s ended before setup lifetime closed", role), err)
				group.mu.Lock()
				group.errs = append(group.errs, err)
				group.mu.Unlock()
				failure.Do(func() { close(group.failed) })
			} else if err != nil {
				group.mu.Lock()
				group.errs = append(group.errs, fmt.Errorf("%s cleanup: %w", role, err))
				group.mu.Unlock()
			}
		}(pdSetupChildRoles[i], child)
	}
	go func() { waits.Wait(); close(group.done) }()
	return group, admission
}
func waitPDSetupChild(pid int) error {
	var status syscall.WaitStatus
	for {
		got, err := syscall.Wait4(pid, &status, 0, nil)
		if err == syscall.EINTR {
			continue
		}
		if err != nil {
			return err
		}
		if got != pid || !status.Exited() || status.ExitStatus() != 0 {
			return fmt.Errorf("setup child wait status %d", status)
		}
		return nil
	}
}

func (g *pdSetupChildren) expectStop() {
	if g != nil {
		g.expected.Store(true)
	}
}
func (g *pdSetupChildren) validate() error {
	if g == nil {
		return nil
	}
	select {
	case <-g.failed:
		return errors.New("setup helper ended before DSP startup")
	default:
		return nil
	}
}
func (g *pdSetupChildren) wait(ctx context.Context) error {
	if g == nil {
		return nil
	}
	select {
	case <-g.done:
		g.mu.Lock()
		defer g.mu.Unlock()
		return errors.Join(g.errs...)
	case <-ctx.Done():
		return fmt.Errorf("setup children not reaped: %w", ctx.Err())
	}
}
