//go:build linux

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
)

const pdReceiveGuardianMode = "--pd-receive-guardian"
const pdReceiveGuardianReady = "{\"event\":\"receive_guardian_ready\",\"rf_commands\":[]}\n"

type pdReceiveGuardianRecord struct {
	Version     int
	ClientID    string
	GUIHandle   uint32
	PanID       uint32
	PanGain     int
	SliceStatus string
}

// The private pipe carries a bounded receive snapshot, never commands, paths,
// PIDs or authority to allocate a receiver or key RF. Reuse the status parser.
func encodePDReceiveGuardian(original pdReceiveOriginal) ([]byte, error) {
	commands, err := pdReceiveRestoreCommands(original)
	if err != nil || !pdReceiveClientIDValid(original.clientID) || original.guiHandle == 0 || original.panID == 0 || original.panGain < -100 || original.panGain > 100 {
		return nil, errors.New("invalid receive guardian snapshot")
	}
	s := original.settings
	status := fmt.Sprintf("S1|slice %d in_use=1 sample_rate=24000 client_handle=0x%08x pan=0x%08x RF_frequency=%d.%06d mode=%s rxant=%s filter_lo=%d filter_hi=%d %s", original.sliceID, original.guiHandle, original.panID, s.frequencyHz/1_000_000, s.frequencyHz%1_000_000, s.mode, s.antenna, s.filterLow, s.filterHigh, strings.TrimPrefix(commands[1], fmt.Sprintf("slice set %d ", original.sliceID)))
	data, err := json.Marshal(pdReceiveGuardianRecord{1, original.clientID, original.guiHandle, original.panID, original.panGain, status})
	if len(data) > 4096 {
		return nil, errors.New("receive guardian snapshot too large")
	}
	return data, err
}

func decodePDReceiveGuardian(data []byte) (pdReceiveOriginal, error) {
	var record pdReceiveGuardianRecord
	if len(data) == 0 || len(data) > 4096 {
		return pdReceiveOriginal{}, errors.New("invalid receive guardian record size")
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&record); err != nil {
		return pdReceiveOriginal{}, err
	}
	if err := decoder.Decode(new(any)); err != io.EOF || record.Version != 1 {
		return pdReceiveOriginal{}, errors.New("invalid receive guardian record framing")
	}
	canonical, err := json.Marshal(record)
	if err != nil || !bytes.Equal(data, canonical) {
		return pdReceiveOriginal{}, errors.New("noncanonical receive guardian record")
	}
	return capturePDReceiveOriginal(record.SliceStatus, record.GUIHandle, record.ClientID, record.PanID, record.PanGain)
}

func startPDReceiveGuardian(program string, life *os.File, original pdReceiveOriginal) (*aetherPDLeaseGuard, error) {
	data, err := encodePDReceiveGuardian(original)
	if err != nil {
		return nil, err
	}
	r, w, err := os.Pipe()
	if err != nil {
		return nil, err
	}
	defer r.Close()
	if n, err := w.Write(data); err != nil || n != len(data) {
		w.Close()
		return nil, errors.New("receive guardian snapshot write failed")
	}
	w.Close()
	cmd := exec.Command(program, pdReceiveGuardianMode)
	cmd.Env = []string{"PATH=/usr/bin:/bin"}
	cmd.ExtraFiles = []*os.File{life, r}
	if os.Geteuid() == 0 {
		cmd.SysProcAttr = &syscall.SysProcAttr{Credential: &syscall.Credential{Uid: 78666, Gid: 20, Groups: []uint32{}}}
	}
	return startPDReadyCommand(cmd, pdReceiveGuardianReady)
}

func pdReceiveGuardianDescriptors() (*os.File, *os.File, error) {
	var stats [2]syscall.Stat_t
	for i, fd := range []int{3, 4} {
		flags, _, e := syscall.Syscall(syscall.SYS_FCNTL, uintptr(fd), syscall.F_GETFL, 0)
		if e != 0 || flags&syscall.O_ACCMODE != syscall.O_RDONLY || syscall.Fstat(fd, &stats[i]) != nil || stats[i].Mode&syscall.S_IFMT != syscall.S_IFIFO {
			return nil, nil, errors.New("invalid private receive guardian pipes")
		}
		syscall.CloseOnExec(fd)
	}
	if stats[0].Dev == stats[1].Dev && stats[0].Ino == stats[1].Ino {
		return nil, nil, errors.New("receive guardian pipes must differ")
	}
	return os.NewFile(3, "pd-receive-lifetime"), os.NewFile(4, "pd-receive-snapshot"), nil
}

func waitPDReceiveGuardianLifetime(life *os.File) error {
	fds := []unix.PollFd{{Fd: int32(life.Fd()), Events: unix.POLLIN}}
	for {
		_, err := unix.Poll(fds, 500)
		if err == unix.EINTR {
			continue
		}
		if err != nil {
			return err
		}
		if fds[0].Revents&(unix.POLLHUP|unix.POLLERR|unix.POLLNVAL) != 0 {
			return nil
		}
		// Never consume lifetime payload. Ignore readability without spinning.
		fds[0].Events = 0
	}
}

func waitPDReceiveGuardianRestore(ctx context.Context, original pdReceiveOriginal, observe func(context.Context, pdReceiveOriginal) (pdReceiveView, error)) error {
	// Mode restoration is asynchronous. A fresh, confirmed DAX collision is
	// a refusal, not a transition the guardian may wait out or overwrite.
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		view, err := observe(ctx, original)
		if err == nil && !view.slices[original.sliceID].present {
			return errors.New("receive guardian cannot allocate a missing receiver")
		}
		if err == nil {
			admission := validateExistingPDReceiveRestore(original, view, time.Now())
			if admission == nil {
				return nil
			}
			if errors.Is(admission, errPDReceiveDAXOccupied) {
				return admission
			}
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(100 * time.Millisecond):
		}
	}
}

func restorePDReceiveAfterOwner(ctx context.Context, original pdReceiveOriginal) error {
	if err := waitPDReceiveGuardianRestore(ctx, original, observePDReceiveWorld); err != nil {
		return err
	}
	writer, err := newPDReceiveWriter(ctx, original)
	if err != nil {
		return err
	}
	defer writer.conn.Close()
	return restoreExistingPDReceive(ctx, original, pdReceiveRestoreIO{
		observe: func(ctx context.Context) (pdReceiveView, error) { return observePDReceiveWorld(ctx, original) },
		write:   writer.command,
		readback: func(ctx context.Context) (pdReceiveObservation, error) {
			return observePDReceive(ctx, original.guiHandle, original.panID, original.sliceID)
		},
	}, func() bool { return ctx.Err() == nil })
}

func receiveGuardianMain() int {
	if os.Geteuid() == 0 {
		return 2
	}
	data, err := os.ReadFile("/proc/self/status")
	if err != nil {
		return 2
	}
	masks := 0
	for _, line := range strings.Split(string(data), "\n") {
		for _, key := range []string{"CapEff:", "CapPrm:", "CapInh:"} {
			if strings.HasPrefix(line, key) {
				n, err := strconv.ParseUint(strings.TrimSpace(strings.TrimPrefix(line, key)), 16, 64)
				if err != nil || n != 0 {
					return 2
				}
				masks++
			}
		}
	}
	if masks != 3 {
		return 2
	}
	if _, _, e := syscall.Syscall6(syscall.SYS_PRCTL, 4, 0, 0, 0, 0, 0); e != 0 {
		return 2
	}
	life, record, err := pdReceiveGuardianDescriptors()
	if err != nil {
		return 2
	}
	defer life.Close()
	defer record.Close()
	data, err = io.ReadAll(io.LimitReader(record, 4097))
	if err != nil {
		return 2
	}
	original, err := decodePDReceiveGuardian(data)
	if err != nil {
		return 2
	}
	record.Close()
	if _, err = io.WriteString(os.Stdout, pdReceiveGuardianReady); err != nil {
		return 3
	}
	os.Stdout.Close()
	if err = waitPDReceiveGuardianLifetime(life); err != nil {
		return 3
	}
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	if err = restorePDReceiveAfterOwner(ctx, original); err != nil {
		fmt.Fprintf(os.Stderr, "AetherPD: receive guardian refused restoration: %v\n", err)
		return 4
	}
	return 0
}

func runPDReceiveGuardianMode() bool {
	if len(os.Args) < 2 || os.Args[1] != pdReceiveGuardianMode {
		return false
	}
	if len(os.Args) != 2 {
		os.Exit(2)
	}
	os.Exit(receiveGuardianMain())
	return true
}
