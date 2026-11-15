package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"strconv"
	"sync"
	"time"
)

const (
	aetherPDCaptureSamples       = 16384
	aetherPDCaptureChunk         = 1024 // Native datagram samples.
	aetherPDCaptureHeader        = 72   // Legacy v1; v2 appends the source ID.
	aetherPDCaptureHeaderV2      = 80
	pdCapturePublicChunk         = 512 // Fits the existing 8 KiB private control reply.
	pdCaptureBegin          byte = 'I'
	pdCaptureReadFirst      byte = 0x80
	pdCaptureReadEnd        byte = 0xa0
)

type aetherPDIQCapture struct {
	Measurement                                             *pdMeasurementSource
	ID, CenterHz, Generation, FirstReceiveNS, LastReceiveNS uint64
	Rate, Stream, Flags                                     uint32
	// Unchanged little-endian float32 I/Q pairs, not calibrated antenna watts.
	IQ []byte
}

type pdCaptureManifest struct {
	Measurement    *pdMeasurementSource `json:"measurement,omitempty"`
	CaptureID      string               `json:"capture_id"`
	Format         string               `json:"format"`
	SampleRate     uint32               `json:"sample_rate_hz"`
	CenterHz       uint64               `json:"center_hz"`
	Samples        int                  `json:"sample_count"`
	ChunkSamples   int                  `json:"chunk_samples"`
	Keyed          bool                 `json:"keyed"`
	Correcting     bool                 `json:"correcting"`
	FirstReceiveNS string               `json:"first_receive_ns"`
	LastReceiveNS  string               `json:"last_receive_ns"`
}

type pdCaptureChunk struct {
	CaptureID string `json:"capture_id"`
	Offset    int    `json:"offset"`
	IQ        []byte `json:"iq_f32le"`
}

// One immutable snapshot per original DSP owner. Neither a new session nor a
// repeated begin can reuse an old public identifier, even if DSP counters reset.
type pdCaptureStore struct {
	mu           sync.Mutex
	closed, busy bool
	id           string
	frame        aetherPDIQCapture
	expires      time.Time
}

func pdCaptureOpcode(op byte) bool {
	return op == pdCaptureBegin || (op >= pdCaptureReadFirst && op < pdCaptureReadEnd)
}

func pdCaptureJSON(code int, value any) pdSessionReply {
	body, err := json.Marshal(value)
	return pdSessionReply{code: code, body: body, err: err}
}

func (s *pdCaptureStore) close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.closed = true
	s.id = ""
	s.frame = aetherPDIQCapture{}
}

func (s *pdCaptureStore) operation(ctx context.Context, op byte,
	read func(context.Context) (aetherPDIQCapture, error), alive func() bool) pdSessionReply {
	unavailable := func() pdSessionReply { return pdCaptureJSON(503, map[string]string{"state": "capture_unavailable"}) }
	if !pdCaptureOpcode(op) || ctx.Err() != nil {
		return unavailable()
	}
	s.mu.Lock()
	if s.closed || !alive() {
		s.mu.Unlock()
		return unavailable()
	}
	if op != pdCaptureBegin {
		defer s.mu.Unlock()
		if s.id == "" || !time.Now().Before(s.expires) {
			s.id = ""
			s.frame = aetherPDIQCapture{}
			return unavailable()
		}
		offset := int(op-pdCaptureReadFirst) * pdCapturePublicChunk
		return pdCaptureJSON(200, pdCaptureChunk{s.id, offset, s.frame.IQ[8*offset : 8*(offset+pdCapturePublicChunk)]})
	}
	if s.busy {
		s.mu.Unlock()
		return pdCaptureJSON(409, map[string]string{"state": "capture_busy"})
	}
	s.busy = true
	s.id = ""
	s.frame = aetherPDIQCapture{}
	s.mu.Unlock()
	var token [16]byte
	_, err := rand.Read(token[:])
	var frame aetherPDIQCapture
	if err == nil {
		frame, err = read(ctx)
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.busy = false
	if err != nil || ctx.Err() != nil || s.closed || !alive() || len(frame.IQ) != 8*aetherPDCaptureSamples {
		return unavailable()
	}
	s.id = hex.EncodeToString(token[:])
	s.frame = frame
	s.expires = time.Now().Add(10 * time.Second)
	return pdCaptureJSON(200, pdCaptureManifest{
		Measurement: frame.Measurement, CaptureID: s.id, Format: "f32le-iq-v1", SampleRate: frame.Rate, CenterHz: frame.CenterHz,
		Samples: aetherPDCaptureSamples, ChunkSamples: pdCapturePublicChunk,
		Keyed: frame.Flags&1 != 0, Correcting: frame.Flags&2 != 0,
		FirstReceiveNS: strconv.FormatUint(frame.FirstReceiveNS, 10), LastReceiveNS: strconv.FormatUint(frame.LastReceiveNS, 10),
	})
}

func (p *aetherPDProcess) captureOperation(ctx context.Context, op byte) pdSessionReply {
	if p == nil {
		return pdCaptureJSON(503, map[string]string{"state": "capture_unavailable"})
	}
	return p.captures.operation(ctx, op, p.readNativeCapture, p.captureAlive)
}
