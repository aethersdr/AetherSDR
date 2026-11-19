//go:build linux

package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"syscall"
	"time"
)

func (p *aetherPDProcess) queryStatus(ctx context.Context) (aetherPDStatus, error) {
	if err := ctx.Err(); err != nil {
		return aetherPDStatus{}, err
	}
	if p != nil {
		select {
		case <-p.done:
			// done publishes the DSP exit and cleanup wait results, including
			// deadlines. A Stop receipt or a closed status socket is insufficient.
			state := "stopped"
			if p.err != nil {
				state = "cleanup_unconfirmed"
				if p.receiveRecoveryIncomplete {
					state = "stopped_receive_unconfirmed"
				}
			}
			return aetherPDStatus{State: state}, nil
		default:
		}
	}
	data, err := p.readNativeStatus(ctx)
	if err != nil {
		return aetherPDStatus{}, err
	}
	status, err := parseAetherPDStatus(data)
	if err == nil {
		p.observePDReceiveOwnership(ctx, status)
	}
	return status, err
}

// The inherited socket is the child identity. No fallback to a shared port is
// permitted after an owned session stops, times out or returns malformed data.
func (p *aetherPDProcess) readNativeStatus(ctx context.Context) ([]byte, error) {
	data, _, err := p.readNativeQuery(ctx, '?', 0, 0)
	return data, err
}

// Both protocols share the socket and counter lock, so a status poll cannot
// consume a capture reply (or vice versa).
func (p *aetherPDProcess) readNativeQuery(ctx context.Context, operation byte, id uint64, offset uint32) (data []byte, captureID uint64, err error) {
	if p == nil || p.status == nil || p.statusReads == nil {
		return nil, 0, errors.New("private AetherPD status unavailable")
	}
	if operation != '?' && operation != 'B' && operation != 'R' && operation != 'G' && operation != 'H' {
		return nil, 0, errors.New("invalid native query")
	}
	if (operation == 'G' && (offset < 1 || offset > aetherPDCaptureBatchMax)) ||
		(operation == 'H' && (id == 0 || offset >= aetherPDCaptureBatchMax*aetherPDCaptureSamples || offset%aetherPDCaptureChunk != 0)) {
		return nil, 0, errors.New("invalid native batch query")
	}
	ctx, cancel := context.WithTimeout(ctx, time.Second)
	defer cancel()
	select {
	case p.statusReads <- struct{}{}:
		defer func() { <-p.statusReads }()
	case <-ctx.Done():
		return nil, 0, ctx.Err()
	}
	select {
	case <-p.done:
		return nil, 0, errors.New("owned AetherPD exited")
	default:
	}
	deadline, _ := ctx.Deadline()
	if err := p.status.SetDeadline(deadline); err != nil {
		return nil, 0, err
	}
	interrupted := make(chan struct{})
	stop := context.AfterFunc(ctx, func() { _ = p.status.SetDeadline(time.Now()); close(interrupted) })
	defer func() {
		if !stop() {
			<-interrupted
		}
	}()
	if p.statusNonce == ^uint64(0) {
		return nil, 0, errors.New("status request counter exhausted")
	}
	p.statusNonce++
	request := make([]byte, 9)
	request[0] = operation
	binary.BigEndian.PutUint64(request[1:], p.statusNonce)
	if operation == 'B' || operation == 'G' {
		id = p.statusNonce
	}
	if operation != '?' {
		request = binary.BigEndian.AppendUint64(request, id)
	}
	if operation == 'R' || operation == 'G' || operation == 'H' {
		request = binary.BigEndian.AppendUint32(request, offset)
	}
	if _, err := p.status.Write(request); err != nil {
		return nil, 0, err
	}
	buffer := make([]byte, aetherPDMaxStatusBytes+9)
	for attempt := 0; attempt < 16; attempt++ {
		size, _, flags, _, err := p.status.ReadMsgUnix(buffer, nil)
		if err != nil {
			return nil, 0, err
		}
		if flags&syscall.MSG_TRUNC != 0 || size > aetherPDMaxStatusBytes+8 || size <= 8 {
			return nil, 0, errors.New("invalid private status size")
		}
		if bytes.Equal(buffer[:8], request[1:9]) {
			select {
			case <-p.done:
				return nil, 0, errors.New("owned AetherPD exited")
			case <-ctx.Done():
				return nil, 0, ctx.Err()
			default:
			}
			return buffer[8:size], id, nil
		}
	}
	return nil, 0, errors.New("private status response counter mismatch")
}
