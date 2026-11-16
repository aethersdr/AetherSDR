//go:build linux

package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"time"
)

const aetherPDCaptureBatchMax = 4
const aetherPDCaptureBatchHeader = 104

type pdNativeCaptureQuery func(context.Context, byte, uint64, uint32) ([]byte, uint64, error)

func parseAetherPDCaptureBatch(data []byte, id uint64, offset, count uint32) (aetherPDIQCapture, uint32, error) {
	if count < 1 || count > aetherPDCaptureBatchMax || offset >= count*aetherPDCaptureSamples ||
		len(data) < aetherPDCaptureBatchHeader || string(data[:8]) != "PDIQ0005" ||
		binary.BigEndian.Uint32(data[96:100]) != offset/aetherPDCaptureSamples ||
		binary.BigEndian.Uint32(data[100:104]) != count {
		return aetherPDIQCapture{}, 0, errors.New("invalid private IQ batch")
	}
	return parseAetherPDCapturePayload(data, id, offset, aetherPDCaptureBatchHeader)
}

func samePDCaptureBatchSource(a, b aetherPDIQCapture) bool {
	return a.ID == b.ID && a.CenterHz == b.CenterHz && a.Generation == b.Generation &&
		a.Rate == b.Rate && a.Stream == b.Stream && a.Flags == b.Flags &&
		a.Binding != nil && b.Binding != nil && *a.Binding == *b.Binding &&
		a.ToneConfig != nil && b.ToneConfig != nil && *a.ToneConfig == *b.ToneConfig &&
		a.Measurement != nil && b.Measurement != nil && *a.Measurement == *b.Measurement
}

func (p *aetherPDProcess) readNativeCaptureBatch(ctx context.Context, count uint32) ([]aetherPDIQCapture, error) {
	ctx, cancel := context.WithTimeout(ctx, 4*time.Second)
	defer cancel()
	return readPDCaptureBatch(ctx, count, p.readNativeQuery)
}

// Native acquisition proceeds independently of these reads. The bounded owner
// query validates process lifetime and serializes requests with status polling.
func readPDCaptureBatch(ctx context.Context, count uint32, query pdNativeCaptureQuery) ([]aetherPDIQCapture, error) {
	if count < 1 || count > aetherPDCaptureBatchMax || query == nil {
		return nil, errors.New("invalid IQ batch request")
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	data, id, err := query(ctx, 'G', 0, count)
	if err != nil {
		return nil, err
	}
	frames := make([]aetherPDIQCapture, 0, count)
	var current aetherPDIQCapture
	var metadata []byte
	for offset := uint32(0); offset < count*aetherPDCaptureSamples; {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		frame, state, err := parseAetherPDCaptureBatch(data, id, offset, count)
		if err != nil {
			return nil, err
		}
		switch state {
		case 1:
			if offset%aetherPDCaptureSamples != 0 {
				return nil, errors.New("IQ batch frame reverted to pending")
			}
			timer := time.NewTimer(20 * time.Millisecond)
			select {
			case <-timer.C:
			case <-ctx.Done():
				timer.Stop()
				return nil, ctx.Err()
			}
		case 2:
			if frame.Flags&1 == 0 || frame.Measurement == nil || !frame.Measurement.Tones ||
				frame.ToneConfig == nil || frame.Binding == nil {
				return nil, errors.New("IQ batch lacks ready measurement identity")
			}
			if offset%aetherPDCaptureSamples == 0 {
				if len(frames) != 0 {
					previous := frames[len(frames)-1]
					if !samePDCaptureBatchSource(frames[0], frame) || frame.FirstReceiveNS < previous.LastReceiveNS ||
						frame.FirstReceiveNS-previous.LastReceiveNS > uint64(100*time.Millisecond) ||
						frame.LastReceiveNS-frames[0].FirstReceiveNS > uint64(5*time.Second) {
						return nil, errors.New("IQ batch source or continuity changed")
					}
				}
				current = frame
				current.IQ = make([]byte, 0, 8*aetherPDCaptureSamples)
				metadata = append(metadata[:0], data[28:aetherPDCaptureBatchHeader]...)
			} else if !bytes.Equal(metadata, data[28:aetherPDCaptureBatchHeader]) {
				return nil, errors.New("IQ batch frame metadata changed")
			}
			current.IQ = append(current.IQ, frame.IQ...)
			offset += aetherPDCaptureChunk
			if offset%aetherPDCaptureSamples == 0 {
				frames = append(frames, current)
			}
			if offset == count*aetherPDCaptureSamples {
				return frames, nil
			}
		default:
			return nil, errors.New("IQ batch unavailable, discontinuous or expired")
		}
		data, _, err = query(ctx, 'H', id, offset)
		if err != nil {
			return nil, err
		}
	}
	return nil, errors.New("incomplete IQ batch")
}
