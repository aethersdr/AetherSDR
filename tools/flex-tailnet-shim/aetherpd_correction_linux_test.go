//go:build linux

package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"
)

func correctionStatus(t *testing.T, changes map[string]any) string {
	t.Helper()
	var fields map[string]any
	if err := json.Unmarshal([]byte(pdMeasurementNativeStatus), &fields); err != nil {
		t.Fatal(err)
	}
	fields["correction_control"] = true
	for key, value := range changes {
		fields[key] = value
	}
	encoded, err := json.Marshal(fields)
	if err != nil {
		t.Fatal(err)
	}
	return string(encoded)
}

func TestPDCorrectionHTTPAdmission(t *testing.T) {
	for _, op := range []byte{'0', '1'} {
		t.Run(string(op), func(t *testing.T) {
			n := newTestNode(t, "secret")
			p, peer := calibrationPair(t)
			n.pdProcess = p
			measurementStatusFixture(t, p, correctionStatus(t, nil))
			api := NewAPI(n)
			path := "/v1/aetherpd/correction/bypass"
			if op == '1' {
				path = "/v1/aetherpd/correction/enable"
			}
			for _, test := range []struct {
				token, suffix, body string
				code                int
			}{{"bad", "", "", 401}, {"secret", "?command=anything", "", 400}, {"secret", "", `{"enabled":true}`, 400}} {
				got := call(t, api, "POST", path+test.suffix, "192.168.50.20:4000", test.token, test.body)
				if got.Code != test.code {
					t.Fatal(got.Code, got.Body.String())
				}
			}
			peer.SetReadDeadline(time.Now().Add(20 * time.Millisecond))
			if _, err := peer.Read(make([]byte, 16)); err == nil {
				t.Fatal("rejected caller reached native control")
			}
			for result, want := range []int{200, 409, 409, 503, 503} {
				done := make(chan error, 1)
				go func(result byte) {
					peer.SetReadDeadline(time.Now().Add(time.Second))
					request := make([]byte, 16)
					size, err := peer.Read(request)
					if err == nil && (size != 9 || request[0] != op) {
						err = io.ErrUnexpectedEOF
					}
					if err == nil {
						_, err = peer.Write(append(append([]byte{}, request[1:9]...), result))
					}
					done <- err
				}(byte(result))
				got := call(t, api, "POST", path, "192.168.50.20:4000", "secret", "")
				if err := <-done; err != nil {
					t.Fatal(err)
				}
				if got.Code != want {
					t.Fatal(got.Code, got.Body.String())
				}
				if result == 0 && !strings.Contains(got.Body.String(), "correction_") {
					t.Fatal("wrong receipt", got.Body.String())
				}
			}
		})
	}
}

func TestPDCorrectionStatusAdmission(t *testing.T) {
	for name, change := range map[string]map[string]any{
		"legacy": {"correction_control": false}, "keyed": {"keyed": true},
		"armed":        {"calibration_state": "armed", "calibration_samples_left": 192000},
		"running":      {"calibration_state": "running", "calibration_samples_left": 192000, "keyed": true},
		"unregistered": {"registered": false},
	} {
		t.Run(name, func(t *testing.T) {
			n := newTestNode(t, "secret")
			p, peer := calibrationPair(t)
			n.pdProcess = p
			measurementStatusFixture(t, p, correctionStatus(t, change))
			got := call(t, NewAPI(n), "POST", "/v1/aetherpd/correction/bypass", "192.168.50.20:4000", "secret", "")
			if got.Code != 409 && got.Code != 503 {
				t.Fatal(got.Code, got.Body.String())
			}
			peer.SetReadDeadline(time.Now().Add(20 * time.Millisecond))
			if _, err := peer.Read(make([]byte, 16)); err == nil {
				t.Fatal("ineligible status reached native control")
			}
		})
	}
	for _, value := range []any{nil, "true", 1} {
		if _, err := parseAetherPDStatus([]byte(correctionStatus(t, map[string]any{"correction_control": value}))); err == nil {
			t.Fatal("invalid capability accepted")
		}
	}
}

func TestPDCorrectionUncertainReplyRetiresOwnedLifetime(t *testing.T) {
	for _, revoke := range []bool{false, true} {
		t.Run(map[bool]string{false: "cancel", true: "revoke"}[revoke], func(t *testing.T) {
			n := newTestNode(t, "secret")
			p, peer := calibrationPair(t)
			n.pdProcess = p
			measurementStatusFixture(t, p, correctionStatus(t, nil))
			reader, writer, err := os.Pipe()
			if err != nil {
				t.Fatal(err)
			}
			defer reader.Close()
			defer writer.Close()
			p.owner = writer
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			req := httptest.NewRequest("POST", "/v1/aetherpd/correction/bypass", nil).WithContext(ctx)
			req.RemoteAddr = "192.168.50.20:4000"
			req.Header.Set("Authorization", "Bearer secret")
			response := httptest.NewRecorder()
			done := make(chan struct{})
			go func() { NewAPI(n).ServeHTTP(response, req); close(done) }()
			peer.SetReadDeadline(time.Now().Add(time.Second))
			request := make([]byte, 9)
			if size, err := peer.Read(request); err != nil || size != 9 || request[0] != '0' {
				t.Fatal("native selection missing", err)
			}
			if revoke {
				n.mu.Lock()
				n.cfg.TokenSHA256 = sha256Hex("replacement")
				n.mu.Unlock()
				if _, err := peer.Write(append(append([]byte{}, request[1:]...), 0)); err != nil {
					t.Fatal(err)
				}
			} else {
				cancel()
			}
			select {
			case <-done:
			case <-time.After(2 * time.Second):
				t.Fatal("uncertain selection did not finish")
			}
			if response.Code != 401 && response.Code != 503 {
				t.Fatal(response.Code, response.Body.String())
			}
			reader.SetReadDeadline(time.Now().Add(time.Second))
			if _, err := reader.Read(make([]byte, 1)); err != io.EOF {
				t.Fatal("owned lifetime retained after uncertain selection", err)
			}
		})
	}
}

func TestPDCorrectionSessionVersionFence(t *testing.T) {
	calls := 0
	channel, _ := pdSessionTestPair(t, func(_ context.Context, op byte) pdSessionReply {
		calls++
		if !pdCorrectionOpcode(op) {
			t.Error("unexpected opcode")
		}
		return pdSessionReply{code: 200, body: []byte(`{"state":"correction_bypass_accepted"}`)}
	})
	for _, op := range []byte{'0', '1'} {
		if reply := channel.Call(context.Background(), op); reply.err == nil || !channel.alive() {
			t.Fatal("legacy peer received selection or was retired")
		}
	}
	channel.correctionSupported = true
	for _, op := range []byte{'0', '1'} {
		if reply := channel.Call(context.Background(), op); reply.err != nil {
			t.Fatal(reply)
		}
	}
	if calls != 2 {
		t.Fatal("unexpected private command count", calls)
	}
}
