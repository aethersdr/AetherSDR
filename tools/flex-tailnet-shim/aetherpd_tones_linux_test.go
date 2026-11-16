//go:build linux

package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"strings"
	"testing"
	"time"
)

func toneNativeStatus() string {
	return strings.TrimSuffix(pdMeasurementNativeStatus, "}") + `,"measurement":{"source_id":"0000000000000000","tones":false},"measurement_tone_control":true,"measurement_tone_capture_version":3,"measurement_tone_config":{"min_spacing_hz":200,"max_spacing_hz":10000,"center_hz":1305,"spacing_hz":1190,"source_id":"0000000000000000"}}`
}

func TestPDToneNativeStatus(t *testing.T) {
	status, err := parseAetherPDStatus([]byte(toneNativeStatus()))
	if err != nil || !status.MeasurementToneControl || !status.MeasurementToneConfig.valid() {
		t.Fatal(status, err)
	}
	for _, bad := range []string{
		strings.Replace(toneNativeStatus(), `"measurement_tone_capture_version":3,`, "", 1),
		strings.Replace(toneNativeStatus(), `"measurement_tone_capture_version":3`, `"measurement_tone_capture_version":2`, 1),
		strings.Replace(toneNativeStatus(), `"measurement_tone_control":true,`, "", 1),
		strings.Replace(toneNativeStatus(), `"measurement_tone_control":true`, `"measurement_tone_control":null`, 1),
		strings.Replace(toneNativeStatus(), `"measurement_control":true`, `"measurement_control":false`, 1),
		strings.Replace(toneNativeStatus(), `"source_id":"0000000000000000"`, `"source_id":"0000000000000001"`, 1),
	} {
		if _, err := parseAetherPDStatus([]byte(bad)); err == nil {
			t.Fatal("inconsistent tone status accepted", bad)
		}
	}
	var native map[string]json.RawMessage
	_ = json.Unmarshal([]byte(toneNativeStatus()), &native)
	delete(native, "measurement_tone_config")
	bad, _ := json.Marshal(native)
	if _, err := parseAetherPDStatus(bad); err == nil {
		t.Fatal("partial tone capability accepted")
	}
	if status, err := parseAetherPDStatus([]byte(pdMeasurementNativeStatus)); err != nil || status.MeasurementToneControl || status.MeasurementToneConfig != nil {
		t.Fatal("legacy status changed", status, err)
	}
}

func TestPDToneHTTPToNative(t *testing.T) {
	for _, spacing := range []uint32{200, 1001, 1190, 2610, 10000} {
		n := newTestNode(t, "owner")
		p, peer := calibrationPair(t)
		n.pdProcess = p
		measurementStatusFixture(t, p, toneNativeStatus())
		received := make(chan []byte, 1)
		go func() {
			request := make([]byte, 16)
			_ = peer.SetReadDeadline(time.Now().Add(2 * time.Second))
			size, err := peer.Read(request)
			if err != nil {
				received <- nil
				return
			}
			received <- request[:size]
			_, _ = peer.Write(append(append([]byte{}, request[1:9]...), 0))
		}()
		body, _ := json.Marshal(map[string]uint32{"spacing_hz": spacing})
		response := call(t, NewAPI(n), "POST", "/v2/aetherpd/measurement/arm", "192.168.50.20:4000", "owner", string(body))
		request := <-received
		if response.Code != 200 || len(request) != 11 || request[0] != 'Q' || binary.BigEndian.Uint64(request[1:9]) == 0 || uint32(binary.BigEndian.Uint16(request[9:])) != spacing {
			t.Fatal("configured native request", response.Code, response.Body.String(), request)
		}
	}
}

func TestPDToneLegacyNativeRefuses(t *testing.T) {
	n := newTestNode(t, "owner")
	p, peer := calibrationPair(t)
	n.pdProcess = p
	measurementStatusFixture(t, p, pdMeasurementNativeStatus)
	r := call(t, NewAPI(n), "POST", "/v2/aetherpd/measurement/arm", "192.168.50.20:4000", "owner", `{"spacing_hz":1190}`)
	if r.Code != 503 {
		t.Fatal("legacy native admitted configured arm", r.Code)
	}
	_ = peer.SetReadDeadline(time.Now().Add(20 * time.Millisecond))
	if _, err := peer.Read(make([]byte, 16)); err == nil {
		t.Fatal("configured arm reached legacy native")
	}
	for _, spacing := range []uint32{0, 199, 10001, 65536} {
		if _, err := p.calibrateTone(context.Background(), spacing); err == nil {
			t.Fatal("invalid spacing accepted", spacing)
		}
	}
}

func TestPDToneCaptureNativeHeader(t *testing.T) {
	legacy := captureReply(1, 0, 2)
	v3 := make([]byte, len(legacy)+12)
	copy(v3, legacy[:72])
	copy(v3, "PDIQ0003")
	copy(v3[84:], legacy[72:])
	binary.BigEndian.PutUint64(v3[72:80], 31)
	binary.BigEndian.PutUint32(v3[68:72], 7)
	for _, spacing := range []uint32{200, 1001, 1190, 2610, 10000} {
		binary.BigEndian.PutUint32(v3[80:84], spacing)
		c, state, err := parseAetherPDCapture(v3, 1, 0)
		if err != nil || state != 2 || !c.ToneConfig.valid() || c.ToneConfig.SpacingHz != spacing || c.ToneConfig.SourceID != "000000000000001f" {
			t.Fatal(c, state, err)
		}
	}
	for _, spacing := range []uint32{0, 199, 10001} {
		binary.BigEndian.PutUint32(v3[80:84], spacing)
		if _, _, err := parseAetherPDCapture(v3, 1, 0); err == nil {
			t.Fatal("invalid captured spacing", spacing)
		}
	}
	binary.BigEndian.PutUint64(v3[72:80], 0)
	binary.BigEndian.PutUint32(v3[68:72], 3)
	binary.BigEndian.PutUint32(v3[80:84], 1190)
	if _, _, err := parseAetherPDCapture(v3, 1, 0); err == nil {
		t.Fatal("profile without source identity")
	}
}
