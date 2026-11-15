//go:build linux

package main

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
	"time"
)

const pdMeasurementNativeStatus = `{"name":"AetherPD","registered":true,"keyed":false,"correction":true,"correcting":false,"local_iq_fault":false,"native_apd_blocked":false,"tx_deadline_faults":0,"tx_send_errors":0,"tx_rejected":0,"local_iq_stale":0,"calibration_control":true,"calibration_state":"idle","calibration_samples_left":0,"measurement_control":true,"source_purpose":"calibration"}`

// Own inherited status protocol; no simulated radio or RF commands.
func measurementStatusFixture(t *testing.T, p *aetherPDProcess, body string) {
	t.Helper()
	status, peer := privateStatusFixture(t)
	p.status, p.statusReads = status.status, status.statusReads
	done := make(chan struct{})
	go func() {
		defer close(done)
		for {
			request := make([]byte, 10)
			size, err := peer.Read(request)
			if err != nil || size != 9 || request[0] != '?' {
				return
			}
			if _, err := peer.Write(append(append([]byte{}, request[1:9]...), []byte(body)...)); err != nil {
				return
			}
		}
	}()
	t.Cleanup(func() { peer.Close(); <-done })
}

func TestPDMeasurementStatusCapability(t *testing.T) {
	var native map[string]any
	if err := json.Unmarshal([]byte(pdMeasurementNativeStatus), &native); err != nil {
		t.Fatal(err)
	}
	native["calibration_state"], native["calibration_samples_left"], native["source_purpose"] = "armed", 192000, "measurement"
	encoded := func(m map[string]any) []byte {
		b, err := json.Marshal(m)
		if err != nil {
			t.Fatal(err)
		}
		return b
	}
	status, err := parseAetherPDStatus(encoded(native))
	if err != nil || !status.MeasurementControl || status.SourcePurpose != "measurement" || status.State != "measurement_armed" {
		t.Fatal(status, err)
	}
	native["correction"] = false
	status, err = parseAetherPDStatus(encoded(native))
	if err != nil || status.State != "disabled" || status.SourcePurpose != "measurement" {
		t.Fatal(status, err)
	}
	for _, field := range []string{"measurement_control", "source_purpose"} {
		for _, value := range []any{nil, "invalid", 2} {
			bad := make(map[string]any)
			for k, v := range native {
				bad[k] = v
			}
			bad[field] = value
			if _, err := parseAetherPDStatus(encoded(bad)); err == nil {
				t.Fatal("invalid capability accepted", field, value)
			}
		}
		bad := make(map[string]any)
		for k, v := range native {
			bad[k] = v
		}
		delete(bad, field)
		if _, err := parseAetherPDStatus(encoded(bad)); err == nil {
			t.Fatal("partial capability accepted", field)
		}
	}
	native["measurement_control"] = false
	if _, err := parseAetherPDStatus(encoded(native)); err == nil {
		t.Fatal("unsupported measurement source accepted")
	}
	delete(native, "measurement_control")
	delete(native, "source_purpose")
	status, err = parseAetherPDStatus(encoded(native))
	if err != nil || status.MeasurementControl || status.SourcePurpose != "" {
		t.Fatal("legacy status advertised measurement", status, err)
	}
}

func TestPDMeasurementHTTPAdmission(t *testing.T) {
	for _, supported := range []bool{false, true} {
		t.Run(map[bool]string{false: "legacy", true: "supported"}[supported], func(t *testing.T) {
			n := newTestNode(t, "secret")
			p, peer := calibrationPair(t)
			n.pdProcess = p
			body := pdMeasurementNativeStatus
			if !supported {
				body = strings.Replace(body, `,"measurement_control":true,"source_purpose":"calibration"`, "", 1)
			}
			measurementStatusFixture(t, p, body)
			api := NewAPI(n)
			for _, tc := range []struct {
				token, path, body string
				code              int
			}{
				{"bad", "/v1/aetherpd/measurement/arm", "", 401},
				{"secret", "/v1/aetherpd/measurement/arm?gain=1", "", 400},
				{"secret", "/v1/aetherpd/measurement/arm", `{"duration":99}`, 400},
			} {
				if got := call(t, api, "POST", tc.path, "192.168.50.20:4000", tc.token, tc.body); got.Code != tc.code {
					t.Fatal(got.Code)
				}
			}
			nativeDone := make(chan byte, 1)
			if supported {
				go func() {
					request := make([]byte, 10)
					peer.SetReadDeadline(time.Now().Add(time.Second))
					size, err := peer.Read(request)
					if err != nil || size != 9 {
						nativeDone <- 0
						return
					}
					_, _ = peer.Write(append(append([]byte{}, request[1:9]...), 0))
					nativeDone <- request[0]
				}()
			}
			got := call(t, api, "POST", "/v1/aetherpd/measurement/arm", "192.168.50.20:4000", "secret", "")
			if supported {
				if <-nativeDone != 'M' || got.Code != 200 || !strings.Contains(got.Body.String(), "measurement_arm_accepted") {
					t.Fatal(got.Code, got.Body.String())
				}
			} else {
				if got.Code != 503 {
					t.Fatal(got.Code, got.Body.String())
				}
				peer.SetReadDeadline(time.Now().Add(20 * time.Millisecond))
				if _, err := peer.Read(make([]byte, 10)); err == nil {
					t.Fatal("legacy DSP received measurement arm")
				}
			}
		})
	}
}

func TestPDMeasurementSessionVersionFence(t *testing.T) {
	calls := 0
	channel, _ := pdSessionTestPair(t, func(context.Context, byte) pdSessionReply {
		calls++
		return pdSessionReply{code: 200, body: []byte(`{"state":"measurement_arm_accepted"}`)}
	})
	if reply := channel.Call(context.Background(), 'M'); reply.err == nil || !channel.alive() {
		t.Fatal("legacy session accepted measurement or was retired", reply)
	}
	channel.measurementSupported = true
	if reply := channel.Call(context.Background(), 'M'); reply.err != nil || reply.code != 200 {
		t.Fatal(reply)
	}
	if calls != 1 {
		t.Fatal("unsupported measurement reached session", calls)
	}
}

func TestPDMeasurementSourceJSON(t *testing.T) {
	for _, body := range []string{`null`, `{}`, `{"source_id":"0000000000000001","tones":null}`, `{"source_id":"0000000000000000","tones":true}`, `{"source_id":"0000000000000001","tones":true,"tones":false}`, `{"source_id":"0000000000000001","tones":false,"extra":0}`, `{"source_id":"000000000000000F","tones":false}`} {
		if _, err := parsePDMeasurementSource([]byte(body)); err == nil {
			t.Fatal("invalid source metadata accepted", body)
		}
	}
	if got, err := parsePDMeasurementSource([]byte(`{"source_id":"000000000000001f","tones":true}`)); err != nil || got.SourceID != "000000000000001f" || !got.Tones {
		t.Fatal(got, err)
	}
}
