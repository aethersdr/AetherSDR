//go:build linux

package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"time"
)

func (p *aetherPDProcess) captureAlive() bool {
	if p == nil || p.done == nil {
		return false
	}
	select {
	case <-p.done:
		return false
	default:
		return true
	}
}

func parseAetherPDCapture(data []byte, id uint64, offset uint32) (aetherPDIQCapture, uint32, error) {
	bad := errors.New("invalid private IQ snapshot")
	if len(data) < aetherPDCaptureHeader || string(data[:8]) != "PDIQ0001" ||
		id == 0 || binary.BigEndian.Uint64(data[8:16]) != id ||
		binary.BigEndian.Uint32(data[20:24]) != offset || offset >= aetherPDCaptureSamples || offset%aetherPDCaptureChunk != 0 {
		return aetherPDIQCapture{}, 0, bad
	}
	state := binary.BigEndian.Uint32(data[16:20])
	count := binary.BigEndian.Uint32(data[24:28])
	if state < 1 || state > 5 || (state != 2 && (count != 0 || len(data) != aetherPDCaptureHeader)) {
		return aetherPDIQCapture{}, 0, bad
	}
	if state != 2 {
		return aetherPDIQCapture{}, state, nil
	}
	if count != aetherPDCaptureChunk || len(data) != aetherPDCaptureHeader+8*aetherPDCaptureChunk {
		return aetherPDIQCapture{}, 0, bad
	}
	c := aetherPDIQCapture{
		ID: id, Rate: binary.BigEndian.Uint32(data[28:32]), CenterHz: binary.BigEndian.Uint64(data[32:40]),
		Generation: binary.BigEndian.Uint64(data[40:48]), FirstReceiveNS: binary.BigEndian.Uint64(data[48:56]),
		LastReceiveNS: binary.BigEndian.Uint64(data[56:64]), Stream: binary.BigEndian.Uint32(data[64:68]),
		Flags: binary.BigEndian.Uint32(data[68:72]), IQ: data[aetherPDCaptureHeader:],
	}
	if (c.Rate != 24000 && c.Rate != 48000 && c.Rate != 96000 && c.Rate != 192000) ||
		c.CenterHz == 0 || c.CenterHz > 1000000000 || c.Generation == 0 || c.Stream == 0 ||
		c.Flags > 3 || c.Flags == 2 || c.FirstReceiveNS == 0 || c.LastReceiveNS < c.FirstReceiveNS ||
		c.LastReceiveNS-c.FirstReceiveNS > uint64(time.Second) {
		return aetherPDIQCapture{}, 0, bad
	}
	for i := 0; i < len(c.IQ); i += 4 {
		if binary.LittleEndian.Uint32(c.IQ[i:i+4])&0x7f800000 == 0x7f800000 {
			return aetherPDIQCapture{}, 0, bad
		}
	}
	return c, state, nil
}

// Uses only the owned DSP descriptor. No tuning, stream creation, calibration
// or keying occurs. A concurrent newer capture invalidates this retrieval.
func (p *aetherPDProcess) readNativeCapture(ctx context.Context) (aetherPDIQCapture, error) {
	ctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	data, id, err := p.readNativeQuery(ctx, 'B', 0, 0)
	if err != nil {
		return aetherPDIQCapture{}, err
	}
	var result aetherPDIQCapture
	var metadata []byte
	for offset := uint32(0); offset < aetherPDCaptureSamples; {
		chunk, state, err := parseAetherPDCapture(data, id, offset)
		if err != nil {
			return aetherPDIQCapture{}, err
		}
		switch state {
		case 1:
			if offset != 0 {
				return aetherPDIQCapture{}, errors.New("IQ snapshot reverted to pending")
			}
			timer := time.NewTimer(20 * time.Millisecond)
			select {
			case <-timer.C:
			case <-ctx.Done():
				timer.Stop()
				return aetherPDIQCapture{}, ctx.Err()
			case <-p.done:
				timer.Stop()
				return aetherPDIQCapture{}, errors.New("owned AetherPD exited")
			}
		case 2:
			if offset == 0 {
				result = chunk
				result.IQ = make([]byte, 0, 8*aetherPDCaptureSamples)
				metadata = append([]byte(nil), data[28:aetherPDCaptureHeader]...)
			} else if !bytes.Equal(metadata, data[28:aetherPDCaptureHeader]) {
				return aetherPDIQCapture{}, errors.New("IQ snapshot metadata changed")
			}
			result.IQ = append(result.IQ, chunk.IQ...)
			offset += aetherPDCaptureChunk
			if offset == aetherPDCaptureSamples {
				return result, nil
			}
		default:
			return aetherPDIQCapture{}, errors.New("IQ snapshot unavailable, discontinuous or expired")
		}
		data, _, err = p.readNativeQuery(ctx, 'R', id, offset)
		if err != nil {
			return aetherPDIQCapture{}, err
		}
	}
	return aetherPDIQCapture{}, errors.New("incomplete IQ snapshot")
}
