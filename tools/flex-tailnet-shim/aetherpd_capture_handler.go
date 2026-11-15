package main

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"strconv"
	"strings"
	"time"
)

func pdCaptureIDValid(id string) bool {
	if len(id) != 32 || strings.ToLower(id) != id {
		return false
	}
	_, err := hex.DecodeString(id)
	return err == nil
}

func aetherPDCaptureHandler(n *Node, begin bool) http.HandlerFunc {
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
		op := pdCaptureBegin
		id := r.PathValue("captureID")
		index := 0
		if !begin {
			text := r.PathValue("chunk")
			var err error
			index, err = strconv.Atoi(text)
			if !pdCaptureIDValid(id) || err != nil || index < 0 || index >= 32 || strconv.Itoa(index) != text {
				writeJSON(w, 400, apiError{"invalid capture ID or chunk index"})
				return
			}
			op = pdCaptureReadFirst + byte(index)
		}
		unavailable := func() { writeJSON(w, 503, map[string]string{"state": "capture_unavailable"}) }
		if !usable {
			unavailable()
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 4*time.Second)
		defer cancel()
		var reply pdSessionReply
		if channel != nil {
			reply = channel.Call(ctx, op)
		} else {
			reply = process.captureOperation(ctx, op)
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
			var manifest pdCaptureManifest
			if json.Unmarshal(reply.body, &manifest) != nil || !pdCaptureIDValid(manifest.CaptureID) ||
				manifest.Format != "f32le-iq-v1" || manifest.Samples != aetherPDCaptureSamples || manifest.ChunkSamples != pdCapturePublicChunk {
				unavailable()
				return
			}
			writeJSON(w, 200, manifest)
		} else {
			var chunk pdCaptureChunk
			if json.Unmarshal(reply.body, &chunk) != nil || chunk.CaptureID != id || chunk.Offset != index*pdCapturePublicChunk || len(chunk.IQ) != 8*pdCapturePublicChunk {
				unavailable()
				return
			}
			writeJSON(w, 200, chunk)
		}
	}
}
