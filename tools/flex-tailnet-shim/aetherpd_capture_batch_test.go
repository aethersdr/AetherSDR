package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"
)

func batchSessionPair(t *testing.T, operation pdSessionBatchOperation) *pdSessionChannel {
	t.Helper()
	a, b := net.Pipe()
	done := make(chan struct{})
	go func() {
		defer close(done)
		_ = servePDSessionWithBatch(b, func(context.Context, byte) pdSessionReply {
			return pdCaptureJSON(200, map[string]string{"state": "ready"})
		}, func() {}, nil, nil, operation)
	}()
	var hello [8]byte
	if _, err := io.ReadFull(a, hello[:]); err != nil || string(hello[:]) != "PDTALK08" {
		t.Fatal(string(hello[:]), err)
	}
	s := newPDSessionChannel(a)
	s.batchSupported = true
	t.Cleanup(func() { s.Close(); <-done })
	return s
}

func captureBatchStoreFixture(count uint32) []aetherPDIQCapture {
	frames := make([]aetherPDIQCapture, count)
	for i := range frames {
		frame := captureStoreFixture()
		frame.FirstReceiveNS = 1000000000 + uint64(i)*85000000
		frame.LastReceiveNS = frame.FirstReceiveNS + 85000000
		frame.Measurement = &pdMeasurementSource{SourceID: "000000000000001f", Tones: true}
		frame.ToneConfig = pdToneConfig(frame.Measurement.SourceID, 1190)
		frame.Binding = &pdFeedbackBinding{Transport: "local_dax_iq", Channel: 3, Pan: 0x40000002, GUIHandle: 7, Valid: true}
		frames[i] = frame
	}
	return frames
}

func TestPDCaptureBatchPublicLifecycle(t *testing.T) {
	n := newTestNode(t, "owner")
	var store pdCaptureBatchStore
	var calls atomic.Int32
	n.pdSession = batchSessionPair(t, func(ctx context.Context, op byte, value uint16) pdSessionReply {
		calls.Add(1)
		return store.operation(ctx, op, value, func(_ context.Context, count uint32) ([]aetherPDIQCapture, error) {
			return captureBatchStoreFixture(count), nil
		}, func() bool { return true })
	})
	h := NewAPI(n)
	for _, request := range []struct {
		method, path, token, body string
		code                      int
	}{
		{"POST", "/v4/aetherpd/capture/4", "wrong", "", 401},
		{"POST", "/v4/aetherpd/capture/4", "owner", "{}", 400},
		{"POST", "/v4/aetherpd/capture/4?count=2", "owner", "", 400},
		{"POST", "/v4/aetherpd/capture/0", "owner", "", 400},
		{"POST", "/v4/aetherpd/capture/5", "owner", "", 400},
		{"POST", "/v4/aetherpd/capture/04", "owner", "", 400},
		{"GET", "/v4/aetherpd/capture/bad/0/0", "owner", "", 400},
		{"GET", "/v4/aetherpd/capture/00000000000000000000000000000000/4/0", "owner", "", 400},
		{"GET", "/v4/aetherpd/capture/00000000000000000000000000000000/0/32", "owner", "", 400},
	} {
		if response := call(t, h, request.method, request.path, "192.168.50.20:4000", request.token, request.body); response.Code != request.code {
			t.Fatal(request.path, response.Code, response.Body.String())
		}
	}
	if calls.Load() != 0 {
		t.Fatal("invalid requests reached session")
	}
	n.pdSession.batchSupported = false
	if response := call(t, h, "POST", "/v4/aetherpd/capture/4", "192.168.50.20:4000", "owner", ""); response.Code != 501 || calls.Load() != 0 {
		t.Fatal("legacy session used", response.Code)
	}
	n.pdSession.batchSupported = true
	response := call(t, h, "POST", "/v4/aetherpd/capture/4", "192.168.50.20:4000", "owner", "")
	var manifest pdCaptureBatchManifest
	if response.Code != 200 || json.Unmarshal(response.Body.Bytes(), &manifest) != nil || !manifest.valid(4) || response.Body.Len() > pdSessionMaxBody {
		t.Fatal(response.Code, response.Body.String())
	}
	for i := 0; i < 4; i++ {
		for j := 0; j < 32; j++ {
			path := fmt.Sprintf("/v4/aetherpd/capture/%s/%d/%d", manifest.CaptureID, i, j)
			r := call(t, h, "GET", path, "192.168.50.20:4000", "owner", "")
			var chunk pdCaptureBatchChunk
			if r.Code != 200 || json.Unmarshal(r.Body.Bytes(), &chunk) != nil || chunk.Frame != i || chunk.Offset != 512*j || len(chunk.IQ) != 4096 || r.Body.Len() > pdSessionMaxBody {
				t.Fatal(i, j, r.Code)
			}
		}
	}
	oldPath := fmt.Sprintf("/v4/aetherpd/capture/%s/0/0", manifest.CaptureID)
	response = call(t, h, "POST", "/v4/aetherpd/capture/1", "192.168.50.20:4000", "owner", "")
	var replacement pdCaptureBatchManifest
	if response.Code != 200 || json.Unmarshal(response.Body.Bytes(), &replacement) != nil || !replacement.valid(1) || replacement.CaptureID == manifest.CaptureID {
		t.Fatal("replacement reused identity")
	}
	if r := call(t, h, "GET", oldPath, "192.168.50.20:4000", "owner", ""); r.Code != 503 {
		t.Fatal("stale ID read successor")
	}
	missingFrame := fmt.Sprintf("/v4/aetherpd/capture/%s/1/0", replacement.CaptureID)
	if r := call(t, h, "GET", missingFrame, "192.168.50.20:4000", "owner", ""); r.Code != 503 {
		t.Fatal("read outside batch")
	}
	store.mu.Lock()
	store.expires = time.Now().Add(-time.Second)
	store.mu.Unlock()
	path := fmt.Sprintf("/v4/aetherpd/capture/%s/0/0", replacement.CaptureID)
	if r := call(t, h, "GET", path, "192.168.50.20:4000", "owner", ""); r.Code != 503 {
		t.Fatal("expired batch read")
	}
	store.close()
	if r := call(t, h, "POST", "/v4/aetherpd/capture/4", "192.168.50.20:4000", "owner", ""); r.Code != 503 {
		t.Fatal("closed batch restarted")
	}
}

func TestPDCaptureBatchStoreInterrupted(t *testing.T) {
	for _, fault := range []string{"close", "cancel", "owner_exit", "partial", "mixed"} {
		t.Run(fault, func(t *testing.T) {
			var store pdCaptureBatchStore
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			alive := true
			result := store.operation(ctx, pdCaptureBatchBegin, 4, func(ctx context.Context, count uint32) ([]aetherPDIQCapture, error) {
				if reply := store.operation(ctx, pdCaptureBatchBegin, 4, nil, func() bool { return true }); reply.code != 409 {
					t.Fatal("concurrent begin not rejected")
				}
				if reply := store.operation(ctx, pdCaptureBatchRead, 0, nil, func() bool { return true }); reply.code != 503 {
					t.Fatal("pending samples escaped")
				}
				frames := captureBatchStoreFixture(count)
				switch fault {
				case "close":
					store.close()
				case "cancel":
					cancel()
				case "owner_exit":
					alive = false
				case "partial":
					frames = frames[:3]
				case "mixed":
					frames[3].Generation++
				}
				return frames, nil
			}, func() bool { return alive })
			if result.code != 503 || store.id != "" || store.frames != nil {
				t.Fatal("interrupted batch published", result.code)
			}
		})
	}
}

func TestPDCaptureBatchPublicOwnerFence(t *testing.T) {
	for _, change := range []string{"credential", "successor", "stopped", "starting"} {
		t.Run(change, func(t *testing.T) {
			n := newTestNode(t, "owner")
			entered, release := make(chan struct{}), make(chan struct{})
			var store pdCaptureBatchStore
			n.pdSession = batchSessionPair(t, func(ctx context.Context, op byte, value uint16) pdSessionReply {
				close(entered)
				<-release
				return store.operation(ctx, op, value, func(context.Context, uint32) ([]aetherPDIQCapture, error) { return captureBatchStoreFixture(4), nil }, func() bool { return true })
			})
			original := n.pdSession
			h := NewAPI(n)
			request := httptest.NewRequest("POST", "/v4/aetherpd/capture/4", nil)
			request.RemoteAddr = "192.168.50.20:4000"
			request.Header.Set("Authorization", "Bearer owner")
			response := httptest.NewRecorder()
			done := make(chan struct{})
			go func() { defer close(done); h.ServeHTTP(response, request) }()
			<-entered
			n.mu.Lock()
			switch change {
			case "credential":
				n.cfg.TokenSHA256 = sha256Hex("successor")
			case "successor":
				n.pdSession = nil
			case "stopped":
				n.pdSessionStopped = true
			case "starting":
				n.pdStarting = &pdSessionStart{}
			}
			n.mu.Unlock()
			close(release)
			<-done
			want := 503
			if change == "credential" {
				want = 401
			}
			if response.Code != want {
				t.Fatal("retired owner returned batch", response.Code)
			}
			if change == "credential" && original.alive() {
				t.Fatal("revoked channel retained")
			}
		})
	}
}

func TestPDCaptureBatchSessionCancellation(t *testing.T) {
	for _, op := range []byte{pdCaptureBatchBegin, pdCaptureBatchRead} {
		t.Run(string(op), func(t *testing.T) {
			entered, release := make(chan struct{}), make(chan struct{})
			s := batchSessionPair(t, func(context.Context, byte, uint16) pdSessionReply {
				close(entered)
				<-release
				return pdCaptureJSON(503, map[string]string{"state": "capture_unavailable"})
			})
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			done := make(chan pdSessionReply, 1)
			go func() { done <- s.CallBatch(ctx, op, 1) }()
			<-entered
			cancel()
			reply := <-done
			alive, busy := s.alive(), s.busy()
			close(release)
			if reply.err == nil || !alive || !busy {
				t.Fatal("cancel retired session or lost pending slot")
			}
			if reply := s.Call(context.Background(), 'S'); reply.err != nil || reply.code != 200 {
				t.Fatal("status no longer usable", reply.err)
			}
		})
	}
}

func TestPDCaptureBatchSessionBounds(t *testing.T) {
	for _, supported := range []bool{false, true} {
		s := &pdSessionChannel{batchSupported: supported}
		for _, request := range []struct {
			op    byte
			value uint16
		}{{'G', 0}, {'G', 5}, {'H', 128}, {'H', 65535}, {'S', 1}} {
			if s.CallBatch(context.Background(), request.op, request.value).err == nil {
				t.Fatal("invalid batch operation accepted")
			}
		}
		if !supported && s.CallBatch(context.Background(), 'G', 4).err == nil {
			t.Fatal("legacy channel admitted batch")
		}
	}
	for _, value := range []uint16{0, 5, 65535} {
		t.Run(fmt.Sprint(value), func(t *testing.T) {
			a, b := net.Pipe()
			defer a.Close()
			done := make(chan error, 1)
			var calls atomic.Int32
			go func() {
				done <- servePDSessionWithBatch(b, nil, func() {}, nil, nil, func(context.Context, byte, uint16) pdSessionReply {
					calls.Add(1)
					return pdCaptureJSON(200, map[string]string{"state": "wrong"})
				})
			}()
			var hello [8]byte
			if _, err := io.ReadFull(a, hello[:]); err != nil {
				t.Fatal(err)
			}
			var frame [18]byte
			copy(frame[:8], "PDCMD004")
			binary.BigEndian.PutUint32(frame[8:12], 1)
			frame[12] = 'G'
			binary.BigEndian.PutUint16(frame[16:], value)
			if err := pdWriteAll(a, frame[:]); err != nil {
				t.Fatal(err)
			}
			if err := <-done; err == nil || calls.Load() != 0 {
				t.Fatal("invalid request dispatched", err)
			}
		})
	}
}

func TestPDCaptureBatchManifestValidation(t *testing.T) {
	var store pdCaptureBatchStore
	reply := store.operation(context.Background(), 'G', 4, func(context.Context, uint32) ([]aetherPDIQCapture, error) { return captureBatchStoreFixture(4), nil }, func() bool { return true })
	if reply.code != 200 {
		t.Fatal(reply.code)
	}
	for _, field := range []string{"id", "size", "rate", "source", "tone", "binding", "clock", "gap", "count", "unkeyed", "correction"} {
		t.Run(field, func(t *testing.T) {
			var m pdCaptureBatchManifest
			if err := json.Unmarshal(reply.body, &m); err != nil {
				t.Fatal(err)
			}
			f := &m.Frames[3]
			switch field {
			case "id":
				f.CaptureID = "00000000000000000000000000000000"
			case "size":
				f.Samples--
			case "rate":
				f.SampleRate = 48000
			case "source":
				f.Source.Generation = "8"
			case "tone":
				f.ToneConfig = pdToneConfig(f.Measurement.SourceID, 200)
			case "binding":
				f.Binding.GUIHandle++
			case "clock":
				f.FirstReceiveNS = "1254999999"
			case "gap":
				f.FirstReceiveNS = "1355000001"
				f.LastReceiveNS = "1440000001"
			case "count":
				m.Frames = m.Frames[:3]
			case "unkeyed":
				f.Keyed = false
			case "correction":
				f.Correcting = false
			}
			if m.valid(4) {
				t.Fatal("invalid manifest accepted", field)
			}
		})
	}
}
