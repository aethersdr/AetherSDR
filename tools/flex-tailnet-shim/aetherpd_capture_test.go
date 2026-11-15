package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http/httptest"
	"sync/atomic"
	"testing"
	"time"
)

func captureStoreFixture() aetherPDIQCapture {
	iq := make([]byte, 8*aetherPDCaptureSamples)
	for i := range iq {
		iq[i] = byte(i % 251)
	}
	return aetherPDIQCapture{ID: 1, CenterHz: 7150000, Generation: 7, FirstReceiveNS: 1000000000,
		LastReceiveNS: 1085000000, Rate: 192000, Stream: 0x40000001, Flags: 3, IQ: iq}
}

func TestPDCaptureStoreLifecycle(t *testing.T) {
	var store pdCaptureStore
	read := func(context.Context) (aetherPDIQCapture, error) { return captureStoreFixture(), nil }
	alive := func() bool { return true }
	ctx := context.Background()
	if got := store.operation(ctx, pdCaptureReadFirst, read, alive); got.code != 503 {
		t.Fatal(got)
	}
	var first pdCaptureManifest
	reply := store.operation(ctx, pdCaptureBegin, read, alive)
	if reply.code != 200 || json.Unmarshal(reply.body, &first) != nil || !pdCaptureIDValid(first.CaptureID) {
		t.Fatal(reply)
	}
	for index := 0; index < 32; index++ {
		reply = store.operation(ctx, pdCaptureReadFirst+byte(index), read, alive)
		var chunk pdCaptureChunk
		if reply.code != 200 || len(reply.body) > pdSessionMaxBody || json.Unmarshal(reply.body, &chunk) != nil || chunk.CaptureID != first.CaptureID || chunk.Offset != index*512 {
			t.Fatal(index, reply)
		}
		if !bytes.Equal(chunk.IQ, captureStoreFixture().IQ[8*chunk.Offset:8*(chunk.Offset+512)]) {
			t.Fatal("sample bytes changed")
		}
	}
	reply = store.operation(ctx, pdCaptureBegin, read, alive)
	var second pdCaptureManifest
	if json.Unmarshal(reply.body, &second) != nil || second.CaptureID == first.CaptureID {
		t.Fatal("snapshot ID reused")
	}
	store.mu.Lock()
	store.expires = time.Now().Add(-time.Second)
	store.mu.Unlock()
	if reply = store.operation(ctx, pdCaptureReadFirst, read, alive); reply.code != 503 {
		t.Fatal("expired snapshot returned")
	}
	store.close()
	if reply = store.operation(ctx, pdCaptureBegin, read, alive); reply.code != 503 {
		t.Fatal("closed owner restarted capture")
	}
}

func TestPDCaptureStoreCloseDuringRead(t *testing.T) {
	var store pdCaptureStore
	entered, release := make(chan struct{}), make(chan struct{})
	read := func(context.Context) (aetherPDIQCapture, error) {
		close(entered)
		<-release
		return captureStoreFixture(), nil
	}
	result := make(chan pdSessionReply, 1)
	go func() {
		result <- store.operation(context.Background(), pdCaptureBegin, read, func() bool { return true })
	}()
	<-entered
	if got := store.operation(context.Background(), pdCaptureBegin, read, func() bool { return true }); got.code != 409 {
		t.Fatal("overlapping collection admitted")
	}
	store.close()
	close(release)
	if got := <-result; got.code != 503 {
		t.Fatal("closed owner published capture")
	}
	store.mu.Lock()
	defer store.mu.Unlock()
	if store.id != "" || len(store.frame.IQ) != 0 {
		t.Fatal("closed owner retained snapshot")
	}
}

func TestPDCaptureLegacyChannelRefusesWithoutSend(t *testing.T) {
	channel := &pdSessionChannel{}
	for _, op := range []byte{pdCaptureBegin, pdCaptureReadFirst, pdCaptureReadEnd - 1} {
		if reply := channel.Call(context.Background(), op); reply.err == nil {
			t.Fatal("version1 accepted capture")
		}
	}
	// A write would dereference the absent transport; refusal must happen first.
}

func TestPDCaptureCancellationRetainsSession(t *testing.T) {
	entered, release := make(chan struct{}), make(chan struct{})
	channel, _ := pdSessionTestPair(t, func(ctx context.Context, op byte) pdSessionReply {
		if op == pdCaptureBegin {
			close(entered)
			select {
			case <-release:
			case <-ctx.Done():
				return pdSessionReply{err: ctx.Err()}
			}
		}
		return pdCaptureJSON(200, map[string]string{"state": "ok"})
	})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan pdSessionReply, 1)
	go func() { done <- channel.Call(ctx, pdCaptureBegin) }()
	<-entered
	cancel()
	if reply := <-done; reply.err == nil {
		t.Fatal("cancelled capture succeeded")
	}
	if !channel.alive() {
		t.Fatal("cancelled capture closed correction session")
	}
	if reply := channel.Call(context.Background(), 'S'); reply.err != nil || reply.code != 200 {
		t.Fatal("status failed while cancelled capture drained", reply)
	}
	close(release)
	deadline := time.Now().Add(time.Second)
	for channel.busy() && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}
	if channel.busy() || !channel.alive() {
		t.Fatal("late capture reply did not drain without closing session")
	}
	if reply := channel.Call(context.Background(), 'S'); reply.err != nil || reply.code != 200 {
		t.Fatal("late reply corrupted next status request", reply)
	}
}

func TestPDCapturePublicSessionTransfer(t *testing.T) {
	n := newTestNode(t, "owner")
	var store pdCaptureStore
	var calls atomic.Int32
	n.pdSession, _ = pdSessionTestPair(t, func(ctx context.Context, op byte) pdSessionReply {
		calls.Add(1)
		return store.operation(ctx, op, func(context.Context) (aetherPDIQCapture, error) { return captureStoreFixture(), nil }, func() bool { return true })
	})
	h := NewAPI(n)
	for _, test := range []struct {
		method, path, token, body string
		code                      int
	}{
		{"POST", "/v1/aetherpd/capture", "wrong", "", 401},
		{"POST", "/v1/aetherpd/capture?x=1", "owner", "", 400},
		{"POST", "/v1/aetherpd/capture", "owner", "{}", 400},
		{"GET", "/v1/aetherpd/capture/bad/0", "owner", "", 400},
		{"GET", "/v1/aetherpd/capture/0123456789abcdef0123456789abcdef/32", "owner", "", 400},
		{"GET", "/v1/aetherpd/capture/0123456789abcdef0123456789abcdef/01", "owner", "", 400},
	} {
		if got := call(t, h, test.method, test.path, "192.168.50.20:4000", test.token, test.body); got.Code != test.code {
			t.Fatal(test, got.Code)
		}
	}
	if calls.Load() != 0 {
		t.Fatal("invalid request reached session")
	}
	start := call(t, h, "POST", "/v1/aetherpd/capture", "192.168.50.20:4000", "owner", "")
	var manifest pdCaptureManifest
	if start.Code != 200 || json.Unmarshal(start.Body.Bytes(), &manifest) != nil || start.Header().Get("Cache-Control") != "no-store" {
		t.Fatal(start.Code, start.Body.String())
	}
	var frame []byte
	for index := 0; index < 32; index++ {
		response := call(t, h, "GET", fmt.Sprintf("/v1/aetherpd/capture/%s/%d", manifest.CaptureID, index), "192.168.50.20:4000", "owner", "")
		var chunk pdCaptureChunk
		if response.Code != 200 || json.Unmarshal(response.Body.Bytes(), &chunk) != nil || chunk.Offset != index*512 {
			t.Fatal(response.Code, response.Body.String())
		}
		frame = append(frame, chunk.IQ...)
	}
	if !bytes.Equal(frame, captureStoreFixture().IQ) {
		t.Fatal("incomplete or changed frame")
	}
	if got := call(t, h, "POST", "/v1/aetherpd/capture", "192.168.50.20:4000", "owner", ""); got.Code != 200 {
		t.Fatal(got.Code)
	}
	oldPath := "/v1/aetherpd/capture/" + manifest.CaptureID + "/0"
	if got := call(t, h, "GET", oldPath, "192.168.50.20:4000", "owner", ""); got.Code != 503 {
		t.Fatal("old ID returned newer snapshot")
	}
	n.mu.Lock()
	n.pdSessionStopped = true
	n.mu.Unlock()
	before := calls.Load()
	if got := call(t, h, "POST", "/v1/aetherpd/capture", "192.168.50.20:4000", "owner", ""); got.Code != 503 {
		t.Fatal("stopped session captured")
	}
	if calls.Load() != before {
		t.Fatal("stopped session received capture request")
	}
}

func TestPDCapturePublicOwnerFence(t *testing.T) {
	for _, revoke := range []bool{false, true} {
		t.Run(fmt.Sprint("revoke=", revoke), func(t *testing.T) {
			n := newTestNode(t, "owner")
			entered, release := make(chan struct{}), make(chan struct{})
			var store pdCaptureStore
			n.pdSession, _ = pdSessionTestPair(t, func(ctx context.Context, op byte) pdSessionReply {
				close(entered)
				<-release
				return store.operation(ctx, op, func(context.Context) (aetherPDIQCapture, error) { return captureStoreFixture(), nil }, func() bool { return true })
			})
			h := NewAPI(n)
			done := make(chan *httptest.ResponseRecorder, 1)
			go func() { done <- call(t, h, "POST", "/v1/aetherpd/capture", "192.168.50.20:4000", "owner", "") }()
			<-entered
			n.mu.Lock()
			if revoke {
				n.cfg.TokenSHA256 = sha256Hex("replacement")
			} else {
				n.pdSession = nil
			}
			n.mu.Unlock()
			close(release)
			response := <-done
			expected := 503
			if revoke {
				expected = 401
			}
			if response.Code != expected || bytes.Contains(response.Body.Bytes(), []byte("capture_id")) {
				t.Fatal(response.Code, response.Body.String())
			}
		})
	}
}
