package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"os"
	"strings"
	"testing"
	"time"
)

func pdFixture(t *testing.T, name string) []byte {
	t.Helper()
	data, err := os.ReadFile("testdata/aetherpd/" + name + ".json")
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func TestAetherPDMeasuredStates(t *testing.T) {
	for _, tc := range []struct {
		name, state string
		requires    bool
	}{
		{"cold_start", "calibrating", false}, {"correcting", "correcting", false},
		{"warm_correcting", "correcting", false}, {"transport_pending", "calibrating", false}, {"stall_bypassed", "recalibration_required", true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			got, err := parseAetherPDStatus(pdFixture(t, tc.name))
			if err != nil {
				t.Fatal(err)
			}
			if !got.Available || got.State != tc.state || got.CalibrationRequired != tc.requires {
				t.Fatalf("unexpected state: %+v", got)
			}
			encoded, _ := json.Marshal(got)
			for _, private := range []string{"daxiq_stream", "tx_mhz", "mic_peak", "active_slice", "fits", "model_resets"} {
				if strings.Contains(string(encoded), `"`+private+`"`) {
					t.Fatalf("debug field leaked: %s", private)
				}
			}
		})
	}
}

func TestAetherPDRejectsInvalidStatus(t *testing.T) {
	good := string(pdFixture(t, "correcting"))
	for name, data := range map[string]string{
		"missing": `{}`, "null": `null`, "array": `[]`, "duplicate": `{"name":"other","name":"AetherPD"}`,
		"trailing": good + `{}`, "oversize": strings.Repeat(" ", aetherPDMaxStatusBytes+1),
		"wrong_producer":       strings.Replace(good, `"AetherPD"`, `"other"`, 1),
		"fault_and_correcting": strings.Replace(good, `"local_iq_fault": false`, `"local_iq_fault": true`, 1),
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := parseAetherPDStatus([]byte(data)); err == nil {
				t.Fatal("invalid status accepted")
			}
		})
	}
}

type pdReadOnlyConn struct {
	net.Conn
	t *testing.T
}

func (c pdReadOnlyConn) Write([]byte) (int, error) {
	c.t.Error("status query wrote bytes")
	return 0, errors.New("read only")
}

func TestAetherPDLocalReadBoundaries(t *testing.T) {
	payload := pdFixture(t, "correcting")
	dial := func(ctx context.Context, network, address string) (net.Conn, error) {
		if network != "tcp4" || address != aetherPDAddress {
			t.Fatalf("unexpected destination %s %s", network, address)
		}
		client, server := net.Pipe()
		go func() { defer server.Close(); _, _ = server.Write(payload) }()
		return pdReadOnlyConn{client, t}, nil
	}
	got, err := queryAetherPDWithDial(context.Background(), dial)
	if err != nil || !got.Correcting {
		t.Fatalf("read failed: %+v %v", got, err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	blocked := func(context.Context, string, string) (net.Conn, error) {
		a, b := net.Pipe()
		go func() { defer b.Close(); _, _ = io.Copy(io.Discard, b) }()
		cancel()
		return a, nil
	}
	start := time.Now()
	if _, err = queryAetherPDWithDial(ctx, blocked); err == nil {
		t.Fatal("canceled read accepted")
	}
	if time.Since(start) > time.Second {
		t.Fatal("cancellation did not bound read")
	}
}

func TestAetherPDAPIAuthAndRevocation(t *testing.T) {
	n := newTestNode(t, "secret")
	calls := 0
	read := func(context.Context) (aetherPDStatus, error) {
		calls++
		return aetherPDStatus{Available: true, State: "correcting"}, nil
	}
	h := newAPIWithAetherPDReader(n, read)
	for _, token := range []string{"", "wrong"} {
		if got := call(t, h, "GET", "/v1/aetherpd/status", "192.168.50.20:4000", token, ""); got.Code != http.StatusUnauthorized {
			t.Fatalf("missing auth: %d", got.Code)
		}
	}
	if calls != 0 {
		t.Fatal("unauthorized caller reached native service")
	}
	if got := call(t, h, "GET", "/v1/aetherpd/status", "172.30.1.5:4000", "secret", ""); got.Code != http.StatusForbidden {
		t.Fatalf("LAN policy bypassed: %d", got.Code)
	}
	if got := call(t, h, "POST", "/v1/aetherpd/status", "192.168.50.20:4000", "secret", ""); got.Code != http.StatusMethodNotAllowed {
		t.Fatalf("mutation verb accepted: %d", got.Code)
	}
	if calls != 0 {
		t.Fatal("blocked caller reached native service")
	}
	if got := call(t, h, "GET", "/v1/aetherpd/status?address=8.8.8.8:22", "192.168.50.20:4000", "secret", ""); got.Code != http.StatusBadRequest {
		t.Fatal(got.Code)
	}
	if calls != 0 {
		t.Fatal("query parameter request reached native service")
	}
	if got := call(t, h, "GET", "/v1/aetherpd/status", "192.168.50.20:4000", "secret", `{"command":"xmit 1"}`); got.Code != http.StatusBadRequest {
		t.Fatal("body accepted", got.Code)
	}
	if calls != 0 {
		t.Fatal("body reached native service")
	}
	if got := call(t, h, "GET", "/v1/aetherpd/status", "192.168.50.20:4000", "secret", ""); got.Code != http.StatusOK {
		t.Fatal(got.Code)
	}
	revoke := newAPIWithAetherPDReader(n, func(context.Context) (aetherPDStatus, error) {
		n.mu.Lock()
		n.cfg.TokenSHA256 = sha256Hex("rotated")
		n.mu.Unlock()
		return aetherPDStatus{Available: true}, nil
	})
	if got := call(t, revoke, "GET", "/v1/aetherpd/status", "192.168.50.20:4000", "secret", ""); got.Code != http.StatusUnauthorized {
		t.Fatalf("revoked token retrieved state: %d", got.Code)
	}
	unavailable := newAPIWithAetherPDReader(n, func(context.Context) (aetherPDStatus, error) {
		return aetherPDStatus{}, errors.New("sensitive native diagnostic")
	})
	got := call(t, unavailable, "GET", "/v1/aetherpd/status", "192.168.50.20:4000", "rotated", "")
	if got.Code != http.StatusServiceUnavailable || strings.Contains(got.Body.String(), "sensitive") || strings.Contains(got.Body.String(), `"correcting":true`) {
		t.Fatal("unavailable native service misreported", got.Code, got.Body.String())
	}
}

// Explicit opt-in for the receive-only radio integration harness.
func TestAetherPDLiveStatus(t *testing.T) {
	if os.Getenv("AETHER_PD_LIVE_STATUS") != "1" {
		t.Skip("requires a running receive-only AetherPD instance")
	}
	n := &Node{}
	n.cfg.TokenSHA256 = sha256Hex("local-one-run-probe")
	reply := call(t, NewAPI(n), "GET", "/v1/aetherpd/status", "192.168.50.20:4000", "local-one-run-probe", "")
	if reply.Code != http.StatusOK {
		t.Fatalf("native status unavailable: %d %s", reply.Code, reply.Body.String())
	}
	var status aetherPDStatus
	if err := json.Unmarshal(reply.Body.Bytes(), &status); err != nil {
		t.Fatal(err)
	}
	if !status.Available || !status.Registered || status.Keyed || status.Correcting || status.State != "idle" {
		t.Fatalf("unexpected receive-only state: %+v", status)
	}
	t.Logf("real native AetherPD through authenticated shim handler: state=%s, registered=%t, keyed=%t", status.State, status.Registered, status.Keyed)
}

func TestAetherPDTransportRecoveryStatus(t *testing.T) {
	original, err := parseAetherPDStatus(pdFixture(t, "transport_pending"))
	if err != nil || original.CalibrationRequired || original.State != "calibrating" {
		t.Fatalf("measured pending state rejected: %+v %v", original, err)
	}

	var fields map[string]any
	if err := json.Unmarshal(pdFixture(t, "transport_pending"), &fields); err != nil {
		t.Fatal(err)
	}
	for _, key := range []string{"transport_cache_enabled", "local_iq_fault", "registered", "keyed", "correction"} {
		saved, existed := fields[key]
		fields[key] = false
		data, _ := json.Marshal(fields)
		if _, err := parseAetherPDStatus(data); err == nil {
			t.Fatalf("pending recovery accepted without %s", key)
		}
		if existed {
			fields[key] = saved
		} else {
			delete(fields, key)
		}
	}
	for _, value := range []any{nil, "true", 1} {
		fields["transport_pending"] = value
		data, _ := json.Marshal(fields)
		if _, err := parseAetherPDStatus(data); err == nil {
			t.Fatal("invalid pending flag accepted")
		}
	}
	fields["transport_pending"] = false
	data, _ := json.Marshal(fields)
	got, err := parseAetherPDStatus(data)
	if err != nil || !got.CalibrationRequired || got.State != "recalibration_required" {
		t.Fatalf("failed recovery did not require calibration: %+v %v", got, err)
	}
}
