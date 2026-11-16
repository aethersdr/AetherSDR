package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"strconv"
	"sync"
	"time"
)

const aetherPDCaptureBatchMax = 4
const pdCaptureBatchBegin byte = 'G'
const pdCaptureBatchRead byte = 'H'

type pdCaptureBatchManifest struct {
	CaptureID string              `json:"capture_id"`
	Frames    []pdCaptureManifest `json:"frames"`
}

type pdCaptureBatchChunk struct {
	pdCaptureChunk
	Frame int `json:"frame_index"`
}

func pdCaptureBatchOpcode(op byte) bool { return op == pdCaptureBatchBegin || op == pdCaptureBatchRead }

// The private session argument is a frame count for G, or a linear index of
// 512-sample public chunks for H. It never selects a stream or changes RF state.
func pdCaptureBatchRequestValid(op byte, value uint16) bool {
	return (op == pdCaptureBatchBegin && value >= 1 && value <= aetherPDCaptureBatchMax) ||
		(op == pdCaptureBatchRead && value < aetherPDCaptureBatchMax*aetherPDCaptureSamples/pdCapturePublicChunk)
}

func samePDCaptureBatchSource(a, b aetherPDIQCapture) bool {
	return a.ID == b.ID && a.CenterHz == b.CenterHz && a.Generation == b.Generation &&
		a.Rate == b.Rate && a.Stream == b.Stream && a.Flags == b.Flags &&
		a.Binding != nil && b.Binding != nil && *a.Binding == *b.Binding &&
		a.ToneConfig != nil && b.ToneConfig != nil && *a.ToneConfig == *b.ToneConfig &&
		a.Measurement != nil && b.Measurement != nil && *a.Measurement == *b.Measurement
}

// One bounded immutable batch per original process. A replacement invalidates
// the old public identity; a failed read cannot publish a partial batch.
type pdCaptureBatchStore struct {
	mu           sync.Mutex
	closed, busy bool
	id           string
	frames       []aetherPDIQCapture
	expires      time.Time
}

func (s *pdCaptureBatchStore) close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.closed = true
	s.id, s.frames = "", nil
}

func (s *pdCaptureBatchStore) operation(ctx context.Context, op byte, value uint16,
	read func(context.Context, uint32) ([]aetherPDIQCapture, error), alive func() bool) pdSessionReply {
	unavailable := func() pdSessionReply { return pdCaptureJSON(503, map[string]string{"state": "capture_unavailable"}) }
	if !pdCaptureBatchRequestValid(op, value) || ctx.Err() != nil {
		return unavailable()
	}
	s.mu.Lock()
	if s.closed || !alive() {
		s.mu.Unlock()
		return unavailable()
	}
	if op == pdCaptureBatchRead {
		defer s.mu.Unlock()
		index := int(value) / (aetherPDCaptureSamples / pdCapturePublicChunk)
		offset := int(value) % (aetherPDCaptureSamples / pdCapturePublicChunk) * pdCapturePublicChunk
		if s.id == "" || !time.Now().Before(s.expires) {
			s.id, s.frames = "", nil
			return unavailable()
		}
		if index >= len(s.frames) {
			return unavailable()
		}
		return pdCaptureJSON(200, pdCaptureBatchChunk{pdCaptureChunk{s.id, offset,
			s.frames[index].IQ[8*offset : 8*(offset+pdCapturePublicChunk)]}, index})
	}
	if s.busy {
		s.mu.Unlock()
		return pdCaptureJSON(409, map[string]string{"state": "capture_busy"})
	}
	s.busy = true
	s.id, s.frames = "", nil
	s.mu.Unlock()
	var token [16]byte
	_, err := rand.Read(token[:])
	var frames []aetherPDIQCapture
	if err == nil {
		frames, err = read(ctx, uint32(value))
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.busy = false
	if err != nil || ctx.Err() != nil || s.closed || !alive() || len(frames) != int(value) {
		return unavailable()
	}
	id := hex.EncodeToString(token[:])
	manifest := pdCaptureBatchManifest{CaptureID: id, Frames: make([]pdCaptureManifest, 0, len(frames))}
	for i, frame := range frames {
		if len(frame.IQ) != 8*aetherPDCaptureSamples || frame.ID == 0 ||
			frame.Flags&1 == 0 || frame.Flags > 3 || frame.Measurement == nil || !frame.Measurement.Tones ||
			frame.ToneConfig == nil || !frame.ToneConfig.valid() || frame.ToneConfig.SourceID == "0000000000000000" ||
			frame.ToneConfig.SourceID != frame.Measurement.SourceID || frame.Binding == nil ||
			frame.Binding.Transport != "local_dax_iq" || frame.Binding.Channel < 1 || frame.Binding.Channel > 4 ||
			frame.Binding.Pan == 0 || frame.Binding.GUIHandle == 0 || !frame.Binding.Valid || frame.Binding.Failed ||
			frame.Generation == 0 || frame.Stream == 0 || frame.CenterHz == 0 || frame.CenterHz > 1000000000 ||
			(frame.Rate != 24000 && frame.Rate != 48000 && frame.Rate != 96000 && frame.Rate != 192000) ||
			frame.FirstReceiveNS == 0 || frame.LastReceiveNS < frame.FirstReceiveNS || frame.LastReceiveNS-frame.FirstReceiveNS > uint64(time.Second) {
			return unavailable()
		}
		if i > 0 && (!samePDCaptureBatchSource(frames[0], frame) || frame.FirstReceiveNS < frames[i-1].LastReceiveNS ||
			frame.FirstReceiveNS-frames[i-1].LastReceiveNS > uint64(100*time.Millisecond)) {
			return unavailable()
		}
		manifest.Frames = append(manifest.Frames, pdCaptureManifest{
			CaptureID: id, Format: "f32le-iq-v1", Samples: aetherPDCaptureSamples, ChunkSamples: pdCapturePublicChunk,
			SampleRate: frame.Rate, CenterHz: frame.CenterHz, Keyed: true, Correcting: frame.Flags&2 != 0,
			FirstReceiveNS: strconv.FormatUint(frame.FirstReceiveNS, 10), LastReceiveNS: strconv.FormatUint(frame.LastReceiveNS, 10),
			Source:  &pdCaptureSource{Kind: "local_dax_iq", StreamID: frame.Stream, Generation: strconv.FormatUint(frame.Generation, 10)},
			Binding: frame.Binding, ToneConfig: frame.ToneConfig, Measurement: frame.Measurement,
		})
	}
	reply := pdCaptureJSON(200, manifest)
	if reply.err != nil || len(reply.body) > pdSessionMaxBody {
		return unavailable()
	}
	s.id, s.frames, s.expires = id, frames, time.Now().Add(10*time.Second)
	return reply
}

func (p *aetherPDProcess) captureBatchOperation(ctx context.Context, op byte, value uint16) pdSessionReply {
	if p == nil {
		return pdCaptureJSON(503, map[string]string{"state": "capture_unavailable"})
	}
	return p.captureBatches.operation(ctx, op, value, p.readNativeCaptureBatch, p.captureAlive)
}
