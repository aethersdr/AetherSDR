package main

import (
	"context"
	"encoding/json"
	"net/http"
	"strconv"
	"time"
)

func (m pdCaptureBatchManifest) valid(count int) bool {
	if !pdCaptureIDValid(m.CaptureID) || count < 1 || count > aetherPDCaptureBatchMax || len(m.Frames) != count {
		return false
	}
	var previousLast uint64
	for i, f := range m.Frames {
		first, firstErr := strconv.ParseUint(f.FirstReceiveNS, 10, 64)
		last, lastErr := strconv.ParseUint(f.LastReceiveNS, 10, 64)
		if f.CaptureID != m.CaptureID || f.Format != "f32le-iq-v1" || f.Samples != aetherPDCaptureSamples ||
			f.ChunkSamples != pdCapturePublicChunk || !f.Keyed || !f.Source.valid() ||
			f.CenterHz == 0 || f.CenterHz > 1000000000 ||
			(f.SampleRate != 24000 && f.SampleRate != 48000 && f.SampleRate != 96000 && f.SampleRate != 192000) ||
			f.Measurement == nil || !f.Measurement.Tones || f.ToneConfig == nil || !f.ToneConfig.valid() ||
			f.ToneConfig.SourceID == "0000000000000000" || f.ToneConfig.SourceID != f.Measurement.SourceID ||
			f.Binding == nil || f.Binding.Transport != "local_dax_iq" || f.Binding.Channel < 1 || f.Binding.Channel > 4 ||
			f.Binding.Pan == 0 || f.Binding.GUIHandle == 0 || !f.Binding.Valid || f.Binding.Failed ||
			firstErr != nil || lastErr != nil || first == 0 || last < first || last-first > uint64(time.Second) ||
			strconv.FormatUint(first, 10) != f.FirstReceiveNS || strconv.FormatUint(last, 10) != f.LastReceiveNS {
			return false
		}
		if i > 0 {
			original := m.Frames[0]
			if first < previousLast || first-previousLast > uint64(100*time.Millisecond) ||
				f.SampleRate != original.SampleRate || f.CenterHz != original.CenterHz || f.Correcting != original.Correcting ||
				*f.Source != *original.Source || *f.Measurement != *original.Measurement ||
				*f.ToneConfig != *original.ToneConfig || *f.Binding != *original.Binding {
				return false
			}
		}
		previousLast = last
	}
	return true
}

func pdCapturePathIndex(text string, limit int) (int, bool) {
	i, err := strconv.Atoi(text)
	return i, err == nil && i >= 0 && i < limit && strconv.Itoa(i) == text
}

func aetherPDCaptureBatchHandler(n *Node, begin bool) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		token := bearer(r)
		n.mu.Lock()
		authorized := n.checkTokenLocked(token)
		channel, process := n.pdSession, n.pdProcess
		usable := n.pdStarting == nil && !n.pdSessionStopped && (channel != nil || process != nil)
		n.mu.Unlock()
		if !authorized {
			writeJSON(w, 401, apiError{errUnauthorized.Error()})
			return
		}
		if r.URL.RawQuery != "" || r.ContentLength != 0 {
			writeJSON(w, 400, apiError{"capture accepts no query or body"})
			return
		}
		op := pdCaptureBatchBegin
		value, valid := pdCapturePathIndex(r.PathValue("count"), aetherPDCaptureBatchMax+1)
		valid = valid && value > 0
		id := r.PathValue("captureID")
		frame, chunk := 0, 0
		if !begin {
			var frameOK, chunkOK bool
			frame, frameOK = pdCapturePathIndex(r.PathValue("frame"), aetherPDCaptureBatchMax)
			chunk, chunkOK = pdCapturePathIndex(r.PathValue("chunk"), aetherPDCaptureSamples/pdCapturePublicChunk)
			valid = frameOK && chunkOK && pdCaptureIDValid(id)
			op, value = pdCaptureBatchRead, frame*(aetherPDCaptureSamples/pdCapturePublicChunk)+chunk
		}
		if !valid {
			writeJSON(w, 400, apiError{"invalid batch capture request"})
			return
		}
		unavailable := func() { writeJSON(w, 503, map[string]string{"state": "capture_unavailable"}) }
		if !usable {
			unavailable()
			return
		}
		if channel != nil && !channel.batchSupported {
			writeJSON(w, 501, apiError{"batch capture is not supported by this session"})
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 4*time.Second)
		defer cancel()
		var reply pdSessionReply
		if channel != nil {
			reply = channel.CallBatch(ctx, op, uint16(value))
		} else {
			reply = process.captureBatchOperation(ctx, op, uint16(value))
		}
		n.mu.Lock()
		authorized = n.checkTokenLocked(token)
		current := n.pdSession == channel && n.pdProcess == process && n.pdStarting == nil && !n.pdSessionStopped
		n.mu.Unlock()
		if !authorized {
			if channel != nil {
				channel.Close()
			}
			writeJSON(w, 401, apiError{errUnauthorized.Error()})
			return
		}
		if !current || ctx.Err() != nil || reply.err != nil || len(reply.body) > pdSessionMaxBody ||
			(channel != nil && !channel.alive()) || (channel == nil && !process.captureAlive()) {
			unavailable()
			return
		}
		if reply.code != 200 {
			if reply.code == 409 {
				writeJSON(w, 409, map[string]string{"state": "capture_busy"})
			} else {
				unavailable()
			}
			return
		}
		if begin {
			var manifest pdCaptureBatchManifest
			if json.Unmarshal(reply.body, &manifest) != nil || !manifest.valid(value) {
				unavailable()
				return
			}
			writeJSON(w, 200, manifest)
		} else {
			var payload pdCaptureBatchChunk
			if json.Unmarshal(reply.body, &payload) != nil || payload.CaptureID != id || payload.Frame != frame ||
				payload.Offset != chunk*pdCapturePublicChunk || len(payload.IQ) != 8*pdCapturePublicChunk {
				unavailable()
				return
			}
			writeJSON(w, 200, payload)
		}
	}
}
