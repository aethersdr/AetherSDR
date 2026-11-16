package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http/httptest"
	"sync/atomic"
	"testing"
)

func TestPDCaptureSourceVersions(t *testing.T) {
	var store pdCaptureStore
	read := func(context.Context) (aetherPDIQCapture, error) { return captureStoreFixture(), nil }
	for _, op := range []byte{pdCaptureBegin, pdCaptureBeginWithSource} {
		reply := store.operation(context.Background(), op, read, func() bool { return true })
		var manifest pdCaptureManifest
		if reply.code != 200 || json.Unmarshal(reply.body, &manifest) != nil {
			t.Fatal(reply)
		}
		if op == pdCaptureBegin {
			if manifest.Source != nil || bytes.Contains(reply.body, []byte(`"source"`)) {
				t.Fatal("legacy capture response changed")
			}
		} else if !manifest.Source.valid() || manifest.Source.StreamID != captureStoreFixture().Stream || manifest.Source.Generation != "7" {
			t.Fatal("source does not identify the sampled stream", string(reply.body))
		}
	}
	for _, source := range []*pdCaptureSource{nil, {}, {Kind: "audio", StreamID: 1, Generation: "1"},
		{Kind: "local_dax_iq", Generation: "1"}, {Kind: "local_dax_iq", StreamID: 1, Generation: "0"},
		{Kind: "local_dax_iq", StreamID: 1, Generation: "01"}, {Kind: "local_dax_iq", StreamID: 1, Generation: "1e1"},
		{Kind: "local_dax_iq", StreamID: 1, Generation: "18446744073709551616"}} {
		if source.valid() {
			t.Fatal("invalid source identity accepted", source)
		}
	}
	channel := &pdSessionChannel{captureSupported: true}
	if channel.Call(context.Background(), pdCaptureBeginWithSource).err == nil {
		t.Fatal("legacy private session accepted new opcode")
	}
}

func TestPDCaptureSourcePublicCompatibility(t *testing.T) {
	n := newTestNode(t, "owner")
	var store pdCaptureStore
	var calls atomic.Int32
	n.pdSession, _ = pdSessionTestPair(t, func(ctx context.Context, op byte) pdSessionReply {
		calls.Add(1)
		return store.operation(ctx, op, func(context.Context) (aetherPDIQCapture, error) { return captureStoreFixture(), nil }, func() bool { return true })
	})
	h := NewAPI(n)
	for _, request := range []struct {
		path, token, body string
		code              int
	}{
		{"/v2/aetherpd/capture", "wrong", "", 401},
		{"/v2/aetherpd/capture?stream=1", "owner", "", 400},
		{"/v2/aetherpd/capture", "owner", "{}", 400},
		{"/v2/aetherpd/capture", "owner", "", 501},
	} {
		if reply := call(t, h, "POST", request.path, "192.168.50.20:4000", request.token, request.body); reply.Code != request.code {
			t.Fatal(reply.Code, reply.Body.String())
		}
	}
	if calls.Load() != 0 {
		t.Fatal("unsupported or invalid request reached old session")
	}
	n.pdSession.captureSourceSupported = true
	response := call(t, h, "POST", "/v2/aetherpd/capture", "192.168.50.20:4000", "owner", "")
	var manifest pdCaptureManifest
	if response.Code != 200 || json.Unmarshal(response.Body.Bytes(), &manifest) != nil || !manifest.Source.valid() {
		t.Fatal(response.Code, response.Body.String())
	}
	path := fmt.Sprintf("/v1/aetherpd/capture/%s/0", manifest.CaptureID)
	chunk := call(t, h, "GET", path, "192.168.50.20:4000", "owner", "")
	var payload pdCaptureChunk
	if chunk.Code != 200 || json.Unmarshal(chunk.Body.Bytes(), &payload) != nil || !bytes.Equal(payload.IQ, captureStoreFixture().IQ[:4096]) {
		t.Fatal("version2 did not retain the original immutable samples")
	}
	legacy := call(t, h, "POST", "/v1/aetherpd/capture", "192.168.50.20:4000", "owner", "")
	if legacy.Code != 200 || bytes.Contains(legacy.Body.Bytes(), []byte(`"source"`)) {
		t.Fatal("version1 changed")
	}
	if stale := call(t, h, "GET", path, "192.168.50.20:4000", "owner", ""); stale.Code != 503 {
		t.Fatal("old identity adopted new capture")
	}
}

func TestPDCaptureSourceOwnerFence(t *testing.T) {
	for _, revoke := range []bool{false, true} {
		t.Run(fmt.Sprint(revoke), func(t *testing.T) {
			n := newTestNode(t, "owner")
			entered, release := make(chan struct{}), make(chan struct{})
			var store pdCaptureStore
			n.pdSession, _ = pdSessionTestPair(t, func(ctx context.Context, op byte) pdSessionReply {
				close(entered)
				<-release
				return store.operation(ctx, op, func(context.Context) (aetherPDIQCapture, error) { return captureStoreFixture(), nil }, func() bool { return true })
			})
			n.pdSession.captureSourceSupported = true
			h := NewAPI(n)
			done := make(chan *httptest.ResponseRecorder, 1)
			go func() { done <- call(t, h, "POST", "/v2/aetherpd/capture", "192.168.50.20:4000", "owner", "") }()
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
			if response.Code != expected || bytes.Contains(response.Body.Bytes(), []byte(`"source"`)) {
				t.Fatal("retired session published source identity", response.Code, response.Body.String())
			}
		})
	}
}
