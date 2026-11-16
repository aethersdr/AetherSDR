package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

const pdSelectedStartFixture = `{"gui_client_id":"0D3EA0E8-B052-4AC6-9D32-D96918634469","gui_handle":52540638,"iq_channel":3,"feedback_pan":1073741826}`
const pdBindingFixture = `{"transport":"local_dax_iq","channel":3,"pan":1073741826,"gui_handle":52540638,"valid":true,"failed":false}`

func pdBindingStatus(binding string) []byte {
	return []byte(`{"available":true,"feedback_binding":` + binding + `}`)
}
func TestPDSelectedStartBoundary(t *testing.T) {
	for channel := uint32(1); channel <= 4; channel++ {
		body := strings.Replace(pdSelectedStartFixture, `"iq_channel":3`, fmt.Sprintf(`"iq_channel":%d`, channel), 1)
		id, err := decodePDStartVersion(strings.NewReader(body), true)
		if err != nil {
			t.Fatal(err)
		}
		frame, err := id.frame()
		if err != nil || len(frame) != 56 || string(frame[:8]) != "PDSTART2" || binary.BigEndian.Uint32(frame[44:48]) != id.GUIHandle || binary.BigEndian.Uint32(frame[48:52]) != channel || binary.BigEndian.Uint32(frame[52:56]) != id.FeedbackPan {
			t.Fatal(err, frame)
		}
	}
	bad := []string{pdStartFixture, `null`, pdSelectedStartFixture + `{}`, strings.Replace(pdSelectedStartFixture, `}`, `,"iq_channel":3}`, 1), strings.Replace(pdSelectedStartFixture, `}`, `,"command":"sh"}`, 1)}
	for _, value := range []string{"0", "5", "-1", "4294967296", "3.0", "3e0", "null", `"3"`} {
		bad = append(bad, strings.Replace(pdSelectedStartFixture, `"iq_channel":3`, `"iq_channel":`+value, 1))
	}
	for _, value := range []string{"0", "-1", "4294967296", "1.5", "null", `"0x40000002"`} {
		bad = append(bad, strings.Replace(pdSelectedStartFixture, `1073741826`, value, 1))
	}
	n := newTestNode(t, "owner")
	dials := 0
	n.pdSessionDial = func(context.Context, pdStartIdentity) (*pdSessionChannel, error) { dials++; return nil, nil }
	h := NewAPI(n)
	for _, body := range bad {
		if _, err := decodePDStartVersion(strings.NewReader(body), true); err == nil {
			t.Errorf("accepted %s", body)
		}
		if r := call(t, h, "POST", "/v2/aetherpd/start", "192.168.50.20:4000", "owner", body); r.Code != 400 {
			t.Fatal(r.Code, body)
		}
	}
	if r := call(t, h, "POST", "/v1/aetherpd/start", "192.168.50.20:4000", "owner", pdSelectedStartFixture); r.Code != 400 {
		t.Fatal(r.Code)
	}
	if r := call(t, h, "POST", "/v2/aetherpd/start", "192.168.50.20:4000", "wrong", pdSelectedStartFixture); r.Code != 401 {
		t.Fatal(r.Code)
	}
	if dials != 0 {
		t.Fatal("invalid or unauthorized request reached launcher", dials)
	}
	id, _ := decodePDStart(strings.NewReader(pdStartFixture))
	for _, selection := range [][2]uint32{{0, 1}, {1, 0}, {5, 1}, {^uint32(0), 1}} {
		id.IQChannel = selection[0]
		id.FeedbackPan = selection[1]
		if _, err := id.frame(); err == nil {
			t.Fatal("partial or invalid binary selection", selection)
		}
	}
}
func TestPDFeedbackBindingValidation(t *testing.T) {
	raw := pdFixture(t, "cold_start")
	withBinding := func(binding string) []byte {
		return []byte(strings.Replace(string(raw), `{`, `{"feedback_binding":`+binding+`,`, 1))
	}
	status, err := parseAetherPDStatus(withBinding(pdBindingFixture))
	if err != nil || status.FeedbackBinding == nil || status.FeedbackBinding.Channel != 3 || !status.FeedbackBinding.Valid {
		t.Fatal(status, err)
	}
	encoded, _ := json.Marshal(status)
	if !strings.Contains(string(encoded), `"feedback_binding":`+pdBindingFixture) {
		t.Fatal(string(encoded))
	}
	bad := []string{`null`, `{}`, pdBindingFixture + `{}`, strings.Replace(pdBindingFixture, `}`, `,"valid":true}`, 1), strings.Replace(pdBindingFixture, `}`, `,"extra":1}`, 1)}
	for _, pair := range [][2]string{{`"local_dax_iq"`, `"audio"`}, {`"channel":3`, `"channel":0`}, {`"channel":3`, `"channel":5`}, {`"pan":1073741826`, `"pan":0`}, {`"gui_handle":52540638`, `"gui_handle":0`}, {`"valid":true`, `"valid":null`}, {`"failed":false`, `"failed":true`}, {`"pan":1073741826`, `"pan":4294967296`}} {
		bad = append(bad, strings.Replace(pdBindingFixture, pair[0], pair[1], 1))
	}
	for _, binding := range bad {
		if _, err := parseAetherPDStatus(withBinding(binding)); err == nil {
			t.Errorf("accepted binding %s", binding)
		}
	}
	for _, failed := range []bool{false, true} {
		binding := strings.Replace(pdBindingFixture, `"valid":true`, `"valid":false`, 1)
		if failed {
			binding = strings.Replace(binding, `"failed":false`, `"failed":true`, 1)
		}
		status, err := parseAetherPDStatus(withBinding(binding))
		want := "feedback_binding_pending"
		if failed {
			want = "feedback_binding_failed"
		}
		if err != nil || status.State != want {
			t.Fatal(status, err)
		}
		active := strings.Replace(string(pdFixture(t, "correcting")), `{`, `{"feedback_binding":`+binding+`,`, 1)
		if _, err := parseAetherPDStatus([]byte(active)); err == nil {
			t.Fatal("correcting without valid binding")
		}
	}
}
func TestPDSelectedStartRequiresObservedBinding(t *testing.T) {
	cases := map[string]string{
		"matching":      pdBindingFixture,
		"wrong_channel": strings.Replace(pdBindingFixture, `"channel":3`, `"channel":1`, 1),
		"wrong_pan":     strings.Replace(pdBindingFixture, "1073741826", "1073741825", 1),
		"wrong_gui":     strings.Replace(pdBindingFixture, "52540638", "52540639", 1),
		"failed":        strings.Replace(strings.Replace(pdBindingFixture, `"valid":true`, `"valid":false`, 1), `"failed":false`, `"failed":true`, 1),
		"missing":       "null",
	}
	for name, binding := range cases {
		t.Run(name, func(t *testing.T) {
			n := newTestNode(t, "owner")
			dials := 0
			var channel *pdSessionChannel
			var calls atomic.Int32
			n.pdSessionDial = func(ctx context.Context, id pdStartIdentity) (*pdSessionChannel, error) {
				dials++
				if id.IQChannel != 3 || id.FeedbackPan != 1073741826 || id.GUIHandle != 52540638 {
					t.Error(id)
				}
				channel, _ = pdSessionTestPair(t, func(ctx context.Context, op byte) pdSessionReply {
					if op != 'S' {
						t.Error("startup issued mutating operation", op)
					}
					calls.Add(1)
					return pdSessionReply{code: 200, body: pdBindingStatus(binding)}
				})
				return channel, nil
			}
			r := call(t, NewAPI(n), "POST", "/v2/aetherpd/start", "192.168.50.20:4000", "owner", pdSelectedStartFixture)
			if name == "matching" {
				if r.Code != 202 || n.pdSession != channel || !channel.alive() {
					t.Fatal(r.Code)
				}
			} else if r.Code != 503 || n.pdSession != nil || channel.alive() {
				t.Fatal(r.Code)
			}
			if dials != 1 || calls.Load() != 1 {
				t.Fatal("retry or fallback", dials, calls.Load())
			}
		})
	}
}
func TestPDSelectedBindingPendingCancelAndRevocation(t *testing.T) {
	for _, mode := range []string{"ready", "canceled", "revoked"} {
		t.Run(mode, func(t *testing.T) {
			n := newTestNode(t, "owner")
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			var calls atomic.Int32
			var channel *pdSessionChannel
			n.pdSessionDial = func(context.Context, pdStartIdentity) (*pdSessionChannel, error) {
				channel, _ = pdSessionTestPair(t, func(context.Context, byte) pdSessionReply {
					count := calls.Add(1)
					if mode == "canceled" {
						cancel()
					} else if mode == "revoked" {
						n.mu.Lock()
						n.cfg.TokenSHA256 = sha256Hex("replacement")
						n.retirePDSessionLocked()
						n.mu.Unlock()
					}
					binding := pdBindingFixture
					if count == 1 {
						binding = strings.Replace(binding, `"valid":true`, `"valid":false`, 1)
					}
					return pdSessionReply{code: 200, body: pdBindingStatus(binding)}
				})
				return channel, nil
			}
			request := httptest.NewRequest("POST", "/v2/aetherpd/start", strings.NewReader(pdSelectedStartFixture)).WithContext(ctx)
			request.RemoteAddr = "192.168.50.20:4000"
			request.Header.Set("Authorization", "Bearer owner")
			response := httptest.NewRecorder()
			NewAPI(n).ServeHTTP(response, request)
			if channel == nil {
				t.Fatalf("start did not reach fixture: HTTP %d", response.Code)
			}
			if mode == "ready" {
				if response.Code != 202 || calls.Load() != 2 || !channel.alive() {
					t.Fatal(response.Code, calls.Load())
				}
			} else {
				expected := 503
				if mode == "revoked" {
					expected = 401
				}
				if response.Code != expected || channel.alive() || n.pdSession != nil {
					t.Fatal(response.Code, channel.alive())
				}
			}
		})
	}
	id, _ := decodePDStartVersion(strings.NewReader(pdSelectedStartFixture), true)
	channel, _ := pdSessionTestPair(t, func(context.Context, byte) pdSessionReply {
		return pdSessionReply{code: 200, body: pdBindingStatus(strings.Replace(pdBindingFixture, `"valid":true`, `"valid":false`, 1))}
	})
	ctx, cancel := context.WithTimeout(context.Background(), 75*time.Millisecond)
	defer cancel()
	if err := confirmPDSelectedBinding(ctx, channel, id); err != context.DeadlineExceeded {
		t.Fatal(err)
	}
	for _, body := range []string{`{}`, `{"available":false}`, `{"available":true,"available":true,"feedback_binding":` + pdBindingFixture + `}`, string(pdBindingStatus(pdBindingFixture)) + `{}`} {
		if _, err := selectedPDBindingStatus(pdSessionReply{code: http.StatusOK, body: []byte(body)}, id); err == nil {
			t.Fatal("invalid status", body)
		}
	}
}

func TestPDNativeBindingInterop(t *testing.T) {
	directory := os.Getenv("AETHER_PD_BINDING_FIXTURE_DIR")
	parser := os.Getenv("AETHER_PD_START_FRAME_FIXTURE")
	if directory == "" || parser == "" {
		t.Skip("native status and start parser fixtures not supplied")
	}
	output := os.Getenv("AETHER_PD_BINDING_PUBLIC_OUT")
	if output != "" {
		if err := os.MkdirAll(output, 0700); err != nil {
			t.Fatal(err)
		}
	}
	for channel := uint32(1); channel <= 4; channel++ {
		id := pdStartIdentity{GUIClientID: "0D3EA0E8-B052-4AC6-9D32-D96918634469", GUIHandle: 0x11223344, IQChannel: channel, FeedbackPan: 0x40000001}
		frame, err := id.frame()
		if err != nil {
			t.Fatal(err)
		}
		cmd := exec.Command(parser)
		cmd.Stdin = bytes.NewReader(frame)
		decoded, err := cmd.Output()
		var actual pdStartIdentity
		if err != nil || json.Unmarshal(decoded, &actual) != nil || actual != id {
			t.Fatal("native start binding differs", string(decoded), err)
		}
		for phase := 0; phase < 3; phase++ {
			name := fmt.Sprintf("channel-%d-phase-%d.json", channel, phase)
			raw, err := os.ReadFile(filepath.Join(directory, name))
			if err != nil {
				t.Fatal(err)
			}
			status, err := parseAetherPDStatus(raw)
			if err != nil || status.FeedbackBinding == nil || status.FeedbackBinding.Channel != channel || status.FeedbackBinding.Pan != id.FeedbackPan || status.FeedbackBinding.GUIHandle != id.GUIHandle || status.FeedbackBinding.Valid != (phase == 1) || status.FeedbackBinding.Failed != (phase == 2) {
				t.Fatal(name, status, err)
			}
			public, err := json.Marshal(status)
			if err != nil {
				t.Fatal(err)
			}
			ready, err := selectedPDBindingStatus(pdSessionReply{code: 200, body: public}, id)
			if (phase == 2) != (err != nil) || ready != (phase == 1) {
				t.Fatal(name, ready, err)
			}
			if output != "" {
				if err = os.WriteFile(filepath.Join(output, name), public, 0600); err != nil {
					t.Fatal(err)
				}
			}
		}
	}
}
