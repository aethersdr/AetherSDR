//go:build linux

package main

import (
	"context"
	"encoding/binary"
	"net"
	"net/http"
	"testing"
	"time"
)

func calibrationPair(t *testing.T) (*aetherPDProcess, *net.UnixConn) {
	t.Helper()
	parent, child, err := newPDCalibrationChannel()
	if err != nil {
		t.Fatal(err)
	}
	conn, err := net.FileConn(child)
	child.Close()
	if err != nil {
		parent.Close()
		t.Fatal(err)
	}
	peer := conn.(*net.UnixConn)
	t.Cleanup(func() { parent.Close(); peer.Close() })
	return &aetherPDProcess{calibration: parent, calibrationReads: make(chan struct{}, 1), done: make(chan struct{})}, peer
}
func TestAetherPDCalibrationAuthAndAdmission(t *testing.T) {
	n := newTestNode(t, "secret")
	p, peer := calibrationPair(t)
	n.pdProcess = p
	h := NewAPI(n)
	for _, tc := range []struct {
		token, path, body string
		want              int
	}{
		{"bad", "/v1/aetherpd/calibration/arm", "", http.StatusUnauthorized},
		{"secret", "/v1/aetherpd/calibration/arm?seconds=99", "", http.StatusBadRequest},
		{"secret", "/v1/aetherpd/calibration/arm", `{"frequency":710}`, http.StatusBadRequest},
	} {
		got := call(t, h, "POST", tc.path, "192.168.50.20:4000", tc.token, tc.body)
		if got.Code != tc.want {
			t.Fatal(got.Code, tc)
		}
	}
	peer.SetReadDeadline(time.Now().Add(20 * time.Millisecond))
	buf := make([]byte, 16)
	if _, err := peer.Read(buf); err == nil {
		t.Fatal("rejected HTTP request reached native control")
	}
	for index, tc := range []struct {
		path   string
		op     byte
		result byte
		want   int
	}{
		{"arm", 'A', 0, 200}, {"cancel", 'X', 0, 200}, {"arm", 'A', 1, 409}, {"arm", 'A', 2, 409}, {"arm", 'A', 3, 503},
	} {
		done := make(chan error, 1)
		go func(op, result byte, nonce uint64) {
			peer.SetReadDeadline(time.Now().Add(time.Second))
			request := make([]byte, 16)
			size, err := peer.Read(request)
			if err == nil && (size != 9 || request[0] != op || binary.BigEndian.Uint64(request[1:9]) != nonce) {
				err = net.ErrClosed
			}
			if err == nil {
				_, err = peer.Write(append(append([]byte{}, request[1:9]...), result))
			}
			done <- err
		}(tc.op, tc.result, uint64(index+1))
		got := call(t, h, "POST", "/v1/aetherpd/calibration/"+tc.path, "192.168.50.20:4000", "secret", "")
		if err := <-done; err != nil {
			t.Fatal(err)
		}
		if got.Code != tc.want {
			t.Fatal(got.Code, got.Body.String(), tc)
		}
	}
}
func TestAetherPDCalibrationPrivateCounter(t *testing.T) {
	p, peer := calibrationPair(t)
	done := make(chan error, 1)
	go func() {
		request := make([]byte, 9)
		peer.SetReadDeadline(time.Now().Add(time.Second))
		_, err := peer.Read(request)
		if err == nil {
			_, err = peer.Write(make([]byte, 9)) // A stale response cannot authorize a result.
			if err == nil {
				_, err = peer.Write(append(append([]byte{}, request[1:]...), 0))
			}
		}
		done <- err
	}()
	if result, err := p.calibrate(context.Background(), 'A'); err != nil || result != 0 {
		t.Fatal(result, err)
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	p.calibrationNonce = ^uint64(0)
	if _, err := p.calibrate(context.Background(), 'X'); err == nil {
		t.Fatal("counter wrap accepted")
	}
	if _, err := p.calibrate(context.Background(), '!'); err == nil {
		t.Fatal("arbitrary command accepted")
	}
	close(p.done)
	if _, err := p.calibrate(context.Background(), 'A'); err == nil {
		t.Fatal("dead owned child accepted")
	}
	peer.SetReadDeadline(time.Now().Add(20 * time.Millisecond))
	b := make([]byte, 9)
	if _, err := peer.Read(b); err == nil {
		t.Fatal("invalid command was sent")
	}
}
func TestAetherPDCalibrationStatus(t *testing.T) {
	base := []byte(`{"name":"AetherPD","registered":true,"keyed":false,"correction":true,"correcting":false,"local_iq_fault":true,"native_apd_blocked":false,"tx_deadline_faults":0,"tx_send_errors":0,"tx_rejected":0,"local_iq_stale":0}`)
	add := func(extra string) []byte {
		return append(append([]byte{}, base[:len(base)-1]...), []byte(","+extra+"}")...)
	}
	data := add(`"calibration_control":true,"calibration_state":"armed","calibration_samples_left":192000`)
	got, err := parseAetherPDStatus(data)
	if err != nil || !got.CalibrationControl || got.CalibrationState != "armed" {
		t.Fatal(got, err)
	}
	for _, bad := range []string{
		`"calibration_control":true`,
		`"calibration_control":true,"calibration_state":"running","calibration_samples_left":192000`,
		`"calibration_control":true,"calibration_state":"armed","calibration_samples_left":192001`,
		`"calibration_control":true,"calibration_state":"completed","calibration_samples_left":1`,
		`"calibration_control":false,"calibration_state":"armed","calibration_samples_left":192000`,
	} {
		if _, err := parseAetherPDStatus(add(bad)); err == nil {
			t.Fatal("bad calibration status accepted", bad)
		}
	}
}

func TestAetherPDInputHeadroomStatus(t *testing.T) {
	base := `{"name":"AetherPD","registered":true,"keyed":true,"correction":true,"correcting":true,"local_iq_fault":false,"native_apd_blocked":false,"tx_deadline_faults":0,"tx_send_errors":0,"tx_rejected":0,"local_iq_stale":0,"limited_samples":`
	status, err := parseAetherPDStatus([]byte(base + `453}`))
	if err != nil || status.InputLimitedSamples != 453 || !status.Correcting {
		t.Fatal(status, err)
	}
	for _, bad := range []string{`-1}`, `"453"}`, `null}`} {
		if _, err := parseAetherPDStatus([]byte(base + bad)); err == nil {
			t.Fatal("invalid headroom counter accepted", bad)
		}
	}
}
