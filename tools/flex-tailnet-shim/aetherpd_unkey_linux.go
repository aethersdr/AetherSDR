//go:build linux

package main

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strconv"
	"syscall"
	"time"
)

type aetherPDUnkey struct {
	cmd        *exec.Cmd
	owner      uint32
	done       chan struct{}
	err        error
	cleanupDue int64
}

func pdClientHandle(text string) (uint32, error) {
	if len(text) != 10 || text[:2] != "0x" {
		return 0, errors.New("invalid prepared client handle")
	}
	for _, c := range text[2:] {
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
			return 0, errors.New("invalid prepared client handle")
		}
	}
	value, err := strconv.ParseUint(text[2:], 16, 32)
	if err != nil || value == 0 {
		return 0, errors.New("invalid prepared client handle")
	}
	return uint32(value), nil
}

func (g *aetherPDUnkey) wait(ctx context.Context) error {
	select {
	case <-g.done:
		return g.err
	case <-ctx.Done():
		return ctx.Err()
	}
}

func startAetherPDUnkey(program string, api, life *os.File, client, owner uint32) (*aetherPDUnkey, error) {
	return launchAetherPDUnkey(context.Background(), program, api, life, client, owner, 0)
}

func launchAetherPDUnkey(ctx context.Context, program string, api, life *os.File, client, owner uint32, durationMS int) (*aetherPDUnkey, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if durationMS < 0 || durationMS > 15000 || client == 0 || owner == 0 || client == owner || api == nil || life == nil {
		return nil, errors.New("invalid unkey guardian launch")
	}
	st, err := os.Stat(program)
	if err != nil {
		return nil, err
	}
	if !st.Mode().IsRegular() || st.Mode()&(os.ModeSetuid|os.ModeSetgid) != 0 {
		return nil, errors.New("invalid unkey guardian executable")
	}
	args := []string{"3", "4", fmt.Sprintf("0x%08x", client), fmt.Sprintf("0x%08x", owner)}
	if durationMS != 0 {
		args = append(args, strconv.Itoa(durationMS))
	}
	cmd := exec.Command(program, args...)
	cmd.Env = []string{"PATH=/usr/bin:/bin"}
	cmd.ExtraFiles = []*os.File{api, life}
	if os.Geteuid() == 0 {
		cmd.SysProcAttr = &syscall.SysProcAttr{Credential: &syscall.Credential{Uid: 78666, Gid: 20, Groups: []uint32{}}}
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	cmd.Stderr = io.Discard
	if err = cmd.Start(); err != nil {
		stdout.Close()
		return nil, err
	}
	g := &aetherPDUnkey{owner: owner, cmd: cmd, done: make(chan struct{})}
	ready := make(chan error, 1)
	go func() {
		reader := bufio.NewReaderSize(stdout, 256)
		line, readErr := reader.ReadSlice('\n')
		if readErr == nil {
			g.cleanupDue, readErr = pdUnkeyReadiness(line, durationMS)
		}
		ready <- readErr
		if readErr != nil {
			_ = cmd.Process.Kill()
		}
		// Bound diagnostics from the fixed helper; never let an unread stdout pipe
		// block its unkey/exit path. No event is interpreted as permission to key.
		count, drainErr := io.Copy(io.Discard, io.LimitReader(reader, 2049))
		if count > 2048 {
			drainErr = errors.New("unkey guardian output exceeded bound")
			_ = cmd.Process.Kill()
		}
		waitErr := cmd.Wait()
		g.err = errors.Join(readErr, drainErr, waitErr)
		close(g.done)
	}()
	timer := time.NewTimer(3 * time.Second)
	defer timer.Stop()
	select {
	case err = <-ready:
		if err == nil {
			err = ctx.Err()
			select {
			case <-g.done:
				err = errors.New("unkey guardian ended during readiness")
			default:
			}
			if err == nil {
				return g, nil
			}
		}
	case <-ctx.Done():
		err = ctx.Err()
	case <-timer.C:
		err = errors.New("unkey guardian readiness timeout")
	}
	_ = cmd.Process.Kill()
	waitCtx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	_ = g.wait(waitCtx)
	select {
	case <-g.done:
		return nil, err
	default:
		// Preserve the child handle if termination has not been observed. A
		// caller must not replace an unaccounted-for cleanup process.
		return g, errors.Join(err, errors.New("unkey guardian termination pending"))
	}
}
