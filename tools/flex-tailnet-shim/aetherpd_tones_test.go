package main

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func toneSessionPair(t *testing.T, operation pdSessionOperation, tone pdSessionToneOperation) *pdSessionChannel {
	t.Helper()
	a, b := net.Pipe()
	done := make(chan struct{})
	go func() { defer close(done); _ = servePDSessionWithTone(b, operation, func() {}, nil, tone) }()
	var hello [8]byte
	if _, err := io.ReadFull(a, hello[:]); err != nil || string(hello[:]) != "PDTALK07" {
		t.Fatal("tone negotiation", err, string(hello[:]))
	}
	c := newPDSessionChannel(a)
	c.captureSupported, c.captureSourceSupported, c.toneSupported = true, true, true
	t.Cleanup(func() { c.Close(); <-done })
	return c
}

func TestPDToneConfigurationStrictJSON(t *testing.T) {
	c := pdToneConfig("000000000000001f", 1001)
	raw, _ := json.Marshal(c)
	got, err := parsePDToneConfig(raw)
	if err != nil || *got != *c {
		t.Fatal(got, err)
	}
	for _, bad := range []string{
		`null`, `{}`, string(raw) + `{}`,
		strings.Replace(string(raw), `1001`, `null`, 1), strings.Replace(string(raw), `1001`, `1001.5`, 1),
		strings.Replace(string(raw), `1001`, `199`, 1), strings.Replace(string(raw), `1001`, `10001`, 1),
		strings.Replace(string(raw), `1305`, `1306`, 1), strings.Replace(string(raw), `"min_spacing_hz":200`, `"min_spacing_hz":201`, 1),
		strings.Replace(string(raw), `001f`, `001F`, 1), strings.TrimSuffix(string(raw), `}`) + `,"spacing_hz":1190}`,
		strings.TrimSuffix(string(raw), `}`) + `,"gain":1}`,
	} {
		if _, err := parsePDToneConfig([]byte(bad)); err == nil {
			t.Fatal("accepted malformed profile", bad)
		}
	}
}

func TestPDToneCaptureLegacyFence(t *testing.T) {
	for _, spacing := range []uint32{200, 1001, 1190, 2610, 10000} {
		t.Run(strconv.Itoa(int(spacing)), func(t *testing.T) {
			var store pdCaptureStore
			frame := captureStoreFixture()
			frame.Measurement = &pdMeasurementSource{SourceID: "000000000000001f", Tones: true}
			frame.ToneConfig = pdToneConfig(frame.Measurement.SourceID, spacing)
			read := func(context.Context) (aetherPDIQCapture, error) { return frame, nil }
			for _, op := range []byte{pdCaptureBeginWithTones, pdCaptureBegin, pdCaptureBeginWithSource} {
				reply := store.operation(context.Background(), op, read, func() bool { return true })
				if op != pdCaptureBeginWithTones && spacing != 1190 {
					if reply.code != 503 || store.id != "" || len(store.frame.IQ) != 0 {
						t.Fatal("legacy capture accepted nondefault source", reply.code, string(reply.body))
					}
					continue
				}
				var m pdCaptureManifest
				if reply.code != 200 || json.Unmarshal(reply.body, &m) != nil {
					t.Fatal(reply)
				}
				if op == pdCaptureBeginWithTones {
					if !m.Source.valid() || !m.ToneConfig.valid() || *m.ToneConfig != *frame.ToneConfig {
						t.Fatal(string(reply.body))
					}
				} else if m.ToneConfig != nil {
					t.Fatal("changed legacy schema")
				}
			}
			frame.ToneConfig.SourceID = "0000000000000020"
			if reply := store.operation(context.Background(), pdCaptureBeginWithTones, read, func() bool { return true }); reply.code != 503 {
				t.Fatal("adopted another source profile")
			}
		})
	}
}

func TestPDToneArmPublicSession(t *testing.T) {
	n := newTestNode(t, "owner")
	var count atomic.Int32
	n.pdSession = toneSessionPair(t, func(context.Context, byte) pdSessionReply {
		t.Error("unexpected legacy operation")
		return pdCaptureJSON(503, map[string]string{"state": "unexpected"})
	}, func(_ context.Context, spacing uint32) pdSessionReply {
		count.Add(1)
		if spacing != 1001 {
			t.Error("spacing changed", spacing)
		}
		return pdCaptureJSON(200, map[string]string{"state": "measurement_arm_accepted"})
	})
	h := NewAPI(n)
	for _, body := range []string{"", `{}`, `null`, `{"spacing_hz":null}`, `{"spacing_hz":199}`, `{"spacing_hz":10001}`, `{"spacing_hz":1001.0}`, `{"spacing_hz":1001,"gain":1}`, `{"spacing_hz":1001,"spacing_hz":1190}`, `{"spacing_hz":1001}{}`, strings.Repeat(" ", 129)} {
		if response := call(t, h, "POST", "/v2/aetherpd/measurement/arm", "192.168.50.20:4000", "owner", body); response.Code != 400 {
			t.Fatal(response.Code, body)
		}
	}
	if response := call(t, h, "POST", "/v2/aetherpd/measurement/arm?gain=1", "192.168.50.20:4000", "owner", `{"spacing_hz":1001}`); response.Code != 400 {
		t.Fatal(response.Code)
	}
	if response := call(t, h, "POST", "/v2/aetherpd/measurement/arm", "192.168.50.20:4000", "wrong", `{"spacing_hz":1001}`); response.Code != 401 {
		t.Fatal(response.Code)
	}
	if count.Load() != 0 {
		t.Fatal("invalid request reached source")
	}
	if response := call(t, h, "POST", "/v2/aetherpd/measurement/arm", "192.168.50.20:4000", "owner", `{"spacing_hz":1001}`); response.Code != 200 {
		t.Fatal(response.Code, response.Body.String())
	}
	if count.Load() != 1 {
		t.Fatal("configured arm not dispatched exactly once")
	}
}

func TestPDToneLegacySessionRefuses(t *testing.T) {
	n := newTestNode(t, "owner")
	n.pdSession, _ = pdSessionTestPair(t, func(context.Context, byte) pdSessionReply {
		t.Error("new operation reached old session")
		return pdCaptureJSON(200, map[string]string{"state": "unexpected"})
	})
	h := NewAPI(n)
	for _, tc := range []struct{ path, body string }{{"/v3/aetherpd/capture", ""}, {"/v2/aetherpd/measurement/arm", `{"spacing_hz":1190}`}} {
		if r := call(t, h, "POST", tc.path, "192.168.50.20:4000", "owner", tc.body); r.Code != 501 {
			t.Fatal(r.Code, r.Body.String())
		}
	}
	if reply := n.pdSession.CallTone(context.Background(), 1190); reply.err == nil {
		t.Fatal("direct unsupported tone call accepted")
	}
	if !n.pdSession.alive() {
		t.Fatal("version refusal retired old session")
	}
}

func TestPDToneArmOwnerAndCancelFence(t *testing.T) {
	for _, change := range []string{"revoke", "replace", "cancel"} {
		t.Run(change, func(t *testing.T) {
			n := newTestNode(t, "owner")
			entered, release := make(chan struct{}), make(chan struct{})
			channel := toneSessionPair(t, nil, func(ctx context.Context, _ uint32) pdSessionReply {
				close(entered)
				select {
				case <-release:
				case <-ctx.Done():
				}
				return pdCaptureJSON(200, map[string]string{"state": "measurement_arm_accepted"})
			})
			n.pdSession = channel
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			r := httptest.NewRequest("POST", "/v2/aetherpd/measurement/arm", strings.NewReader(`{"spacing_hz":1001}`)).WithContext(ctx)
			r.Header.Set("Authorization", "Bearer owner")
			w := httptest.NewRecorder()
			done := make(chan struct{})
			go func() { defer close(done); aetherPDToneArmHandler(n)(w, r) }()
			<-entered
			n.mu.Lock()
			if change == "revoke" {
				n.cfg.TokenSHA256 = sha256Hex("new")
			}
			if change == "replace" {
				n.pdSession = nil
			}
			n.mu.Unlock()
			if change == "cancel" {
				cancel()
			}
			close(release)
			select {
			case <-done:
			case <-time.After(time.Second):
				t.Fatal("abandoned arm did not finish")
			}
			expected := 503
			if change == "revoke" {
				expected = 401
			}
			if w.Code != expected || channel.alive() || bytes.Contains(w.Body.Bytes(), []byte("accepted")) {
				t.Fatal("abandoned arm survived", w.Code, w.Body.String())
			}
		})
	}
}
