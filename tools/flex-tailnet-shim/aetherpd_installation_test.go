package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"net/http"
	"testing"
)

func installationRecord(phase uint32) []byte {
	data := make([]byte, pdInstallationSize)
	copy(data, "PDVIEW01")
	binary.BigEndian.PutUint32(data[8:12], phase)
	binary.BigEndian.PutUint32(data[12:16], 1)
	for i := 16; i < 112; i++ {
		data[i] = byte(i)
	}
	copy(data[112:140], "4.3.16.42034")
	copy(data[140:], "fixture-8600")
	return data
}

func TestPDInstallationParser(t *testing.T) {
	for _, phase := range []uint32{4, 7} {
		st, err := parsePDInstallation(installationRecord(phase))
		if err != nil || st.Committed != (phase == 4) || st.Serial != "fixture-8600" || st.Firmware != "4.3.16.42034" || len(st.PlanDigest) != 64 || len(st.SelectedDigest) != 64 || len(st.SelectedKey) != 64 {
			t.Fatalf("phase %d: %+v %v", phase, st, err)
		}
	}
	for size := 0; size < pdInstallationSize; size++ {
		if _, err := parsePDInstallation(installationRecord(4)[:size]); err == nil {
			t.Fatalf("accepted truncated size %d", size)
		}
	}
	cases := map[string]func([]byte) []byte{
		"trailing":         func(b []byte) []byte { return append(b, 0) },
		"magic":            func(b []byte) []byte { b[0]++; return b },
		"pending":          func(b []byte) []byte { b[11] = 3; return b },
		"phase-high":       func(b []byte) []byte { b[8] = 1; return b },
		"profile":          func(b []byte) []byte { b[15] = 3; return b },
		"zero-plan":        func(b []byte) []byte { clear(b[16:48]); return b },
		"zero-release":     func(b []byte) []byte { clear(b[48:80]); return b },
		"zero-key":         func(b []byte) []byte { clear(b[80:112]); return b },
		"empty-firmware":   func(b []byte) []byte { clear(b[112:140]); return b },
		"empty-serial":     func(b []byte) []byte { clear(b[140:]); return b },
		"control":          func(b []byte) []byte { b[140] = '\n'; return b },
		"firmware-padding": func(b []byte) []byte { b[139] = 'x'; return b },
		"serial-padding":   func(b []byte) []byte { b[203] = 'x'; return b },
		"unterminated":     func(b []byte) []byte { copy(b[140:], bytes.Repeat([]byte{'x'}, 64)); return b },
	}
	for name, change := range cases {
		t.Run(name, func(t *testing.T) {
			if _, err := parsePDInstallation(change(installationRecord(4))); err == nil {
				t.Fatal("malformed record accepted")
			}
		})
	}
}

func TestPDInstallationAPI(t *testing.T) {
	const token = "installation-test-token"
	n := newTestNode(t, token)
	reads := 0
	phase := uint32(4)
	unavailable := false
	h := aetherPDInstallationHandler(n, func() (pdInstallation, error) {
		reads++
		if unavailable {
			return pdInstallation{}, errPDInstallationUnavailable
		}
		return parsePDInstallation(installationRecord(phase))
	})
	for _, presented := range []string{"", "wrong"} {
		rec := call(t, h, "GET", "/v1/aetherpd/installation?unexpected=1", "192.168.1.2:123", presented, "")
		if rec.Code != http.StatusUnauthorized || reads != 0 {
			t.Fatal("unauthenticated read")
		}
	}
	for _, request := range [][2]string{{"/v1/aetherpd/installation?path=store", ""}, {"/v1/aetherpd/installation", "body"}} {
		rec := call(t, h, "GET", request[0], "192.168.1.2:123", token, request[1])
		if rec.Code != http.StatusBadRequest || reads != 0 {
			t.Fatal("parameters accepted")
		}
	}
	for _, p := range []uint32{4, 7} {
		phase = p
		rec := call(t, h, "GET", "/v1/aetherpd/installation", "192.168.1.2:123", token, "")
		var status pdInstallation
		if rec.Code != http.StatusOK || json.Unmarshal(rec.Body.Bytes(), &status) != nil || status.Committed != (p == 4) || rec.Header().Get("Cache-Control") != "no-store" {
			t.Fatalf("bad status: %s", rec.Body.String())
		}
	}
	unavailable = true
	rec := call(t, h, "GET", "/v1/aetherpd/installation", "192.168.1.2:123", token, "")
	if rec.Code != http.StatusServiceUnavailable || !bytes.Contains(rec.Body.Bytes(), []byte(`"installation_committed":false`)) || rec.Header().Get("Cache-Control") != "no-store" {
		t.Fatalf("unavailable not explicit: %s", rec.Body.String())
	}
	// Exercise actual routing and the existing LAN/auth middleware as well.
	api := NewAPI(n)
	for _, remote := range []string{"8.8.8.8:123", "192.168.1.2:123"} {
		want := http.StatusUnauthorized
		if remote == "8.8.8.8:123" {
			want = http.StatusForbidden
		}
		if rec := call(t, api, "GET", "/v1/aetherpd/installation", remote, "", ""); rec.Code != want {
			t.Fatalf("route guard: %d want %d", rec.Code, want)
		}
	}
}
