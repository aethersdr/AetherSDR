//go:build linux

package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"net"
	"os"
	"syscall"
	"time"
)

func newPDCalibrationChannel() (*net.UnixConn, *os.File, error) {
	pair, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_DGRAM|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		return nil, nil, err
	}
	parent := os.NewFile(uintptr(pair[0]), "pd-calibration-parent")
	child := os.NewFile(uintptr(pair[1]), "pd-calibration-child")
	conn, err := net.FileConn(parent)
	parent.Close()
	if err != nil {
		child.Close()
		return nil, nil, err
	}
	unix, ok := conn.(*net.UnixConn)
	if !ok {
		conn.Close()
		child.Close()
		return nil, nil, errors.New("invalid calibration socket")
	}
	return unix, child, nil
}
func (p *aetherPDProcess) calibrate(ctx context.Context, opcode byte) (int, error) {
	if opcode != 'A' && opcode != 'M' && opcode != 'X' && !pdCorrectionOpcode(opcode) {
		return 0, errors.New("invalid calibration command")
	}
	if p == nil || p.calibration == nil || p.calibrationReads == nil {
		return 0, errors.New("private calibration unavailable")
	}
	ctx, cancel := context.WithTimeout(ctx, time.Second)
	defer cancel()
	select {
	case p.calibrationReads <- struct{}{}:
		defer func() { <-p.calibrationReads }()
	case <-ctx.Done():
		return 0, ctx.Err()
	}
	if err := ctx.Err(); err != nil {
		return 0, err
	}
	select {
	case <-p.done:
		return 0, errors.New("owned AetherPD exited")
	default:
	}
	deadline, _ := ctx.Deadline()
	if err := p.calibration.SetDeadline(deadline); err != nil {
		return 0, err
	}
	interrupted := make(chan struct{})
	stop := context.AfterFunc(ctx, func() { _ = p.calibration.SetDeadline(time.Now()); close(interrupted) })
	defer func() {
		if !stop() {
			<-interrupted
		}
	}()
	if p.calibrationNonce == ^uint64(0) {
		return 0, errors.New("calibration counter exhausted")
	}
	p.calibrationNonce++
	request := make([]byte, 9)
	request[0] = opcode
	binary.BigEndian.PutUint64(request[1:], p.calibrationNonce)
	if err := ctx.Err(); err != nil {
		return 0, err
	}
	if _, err := p.calibration.Write(request); err != nil {
		return 0, err
	}
	reply := make([]byte, 10)
	for attempt := 0; attempt < 16; attempt++ {
		size, _, flags, _, err := p.calibration.ReadMsgUnix(reply, nil)
		if err != nil {
			return 0, err
		}
		if size != 9 || flags&syscall.MSG_TRUNC != 0 {
			return 0, errors.New("invalid calibration reply")
		}
		if bytes.Equal(reply[:8], request[1:]) {
			if reply[8] > 4 {
				return 0, errors.New("invalid calibration result")
			}
			return int(reply[8]), nil
		}
	}
	return 0, errors.New("calibration reply counter mismatch")
}
