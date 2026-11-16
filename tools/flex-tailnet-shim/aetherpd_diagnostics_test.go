package main

import (
	"context"
	"encoding/json"
	"net/http"
	"os"
	"strings"
	"testing"
)

func diagnosticFixture() pdDiagnostics {
	return pdDiagnostics{Version: 1,
		Alignment: pdDiagnosticRecord{Count: 2, Text: "alignment evidence: reason=tx_dispatch mono_ns=123 delay=-128.000"},
		SourceEnd: pdDiagnosticRecord{Count: 3, Text: "bounded source ended: reason=feedback_invalidated phase=running keyed=1"}}
}

func TestPDDiagnosticsBoundaries(t *testing.T) {
	fixture := diagnosticFixture()
	encoded, _ := json.Marshal(fixture)
	parsed, err := parsePDDiagnostics(encoded)
	if err != nil || *parsed != fixture {
		t.Fatalf("record lost: %+v %v", parsed, err)
	}
	for _, change := range []func(*pdDiagnostics){
		func(d *pdDiagnostics) { d.Version = 2 },
		func(d *pdDiagnostics) { d.Alignment.Count = 0 },
		func(d *pdDiagnostics) { d.SourceEnd.Text = "" },
		func(d *pdDiagnostics) { d.Alignment.Truncated = true },
		func(d *pdDiagnostics) { d.SourceEnd.Text += strings.Repeat("a", 1024) },
		func(d *pdDiagnostics) { d.Alignment.Text += "\n" },
		func(d *pdDiagnostics) { d.Alignment.Text += "\"" },
		func(d *pdDiagnostics) { d.Alignment.Text += "\\" },
		func(d *pdDiagnostics) { d.Alignment.Text += "é" },
	} {
		bad := fixture
		change(&bad)
		raw, _ := json.Marshal(bad)
		if _, err := parsePDDiagnostics(raw); err == nil {
			t.Fatalf("invalid record accepted: %s", raw)
		}
	}
	for _, raw := range []string{
		`null`, `{}`, strings.Replace(string(encoded), `"version":1`, `"version":null`, 1),
		strings.Replace(string(encoded), `"count":2`, `"count":2,"count":2`, 1),
		strings.Replace(string(encoded), `"count":2`, `"count":4294967296`, 1),
		strings.Replace(string(encoded), `"count":2`, `"count":-1`, 1),
		strings.Replace(string(encoded), `"count":2`, `"count":1.5`, 1),
		strings.Replace(string(encoded), `"count":2`, `"count":2,"command":"xmit 1"`, 1),
	} {
		if _, err := parsePDDiagnostics([]byte(raw)); err == nil {
			t.Fatalf("malformed schema accepted: %s", raw)
		}
	}
	for _, r := range []pdDiagnosticRecord{{}, {Count: ^uint32(0), Truncated: true, Text: "alignment evidence: reason=" + strings.Repeat("x", 1023-len("alignment evidence: reason="))}} {
		fixture.Alignment = r
		raw, _ := json.Marshal(fixture)
		if _, err := parsePDDiagnostics(raw); err != nil {
			t.Fatal(err)
		}
	}
}

func TestPDDiagnosticsStatusProjection(t *testing.T) {
	var fields map[string]any
	if err := json.Unmarshal(pdFixture(t, "stall_bypassed"), &fields); err != nil {
		t.Fatal(err)
	}
	fields["diagnostics"] = diagnosticFixture()
	raw, _ := json.Marshal(fields)
	status, err := parseAetherPDStatus(raw)
	if err != nil || status.Diagnostics == nil || status.Correcting || !status.CalibrationRequired {
		t.Fatalf("historical evidence changed policy: %+v %v", status, err)
	}
	n := &Node{}
	n.cfg.TokenSHA256 = sha256Hex("secret")
	h := newAPIWithAetherPDReader(n, func(context.Context) (aetherPDStatus, error) { return status, nil })
	reply := call(t, h, "GET", "/v1/aetherpd/status", "192.168.50.20:4000", "secret", "")
	var got aetherPDStatus
	if reply.Code != http.StatusOK || json.Unmarshal(reply.Body.Bytes(), &got) != nil || got.Diagnostics == nil || *got.Diagnostics != *status.Diagnostics {
		t.Fatal("authenticated projection lost evidence", reply.Body.String())
	}
}

// Optional composition check consumes output from the native packet-path test.
func TestPDDiagnosticsNativeProjection(t *testing.T) {
	path := os.Getenv("AETHER_PD_DIAGNOSTIC_NATIVE")
	if path == "" {
		t.Skip("requires native packet-path fixture")
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	status, err := parseAetherPDStatus(raw)
	if err != nil || status.Diagnostics == nil || (status.Diagnostics.SourceEnd.Count == 0 && status.Diagnostics.Alignment.Count == 0) || status.Correcting {
		t.Fatalf("native record rejected: %+v %v", status, err)
	}
	encoded, err := json.Marshal(status)
	if err != nil {
		t.Fatal(err)
	}
	if out := os.Getenv("AETHER_PD_DIAGNOSTIC_PUBLIC"); out != "" {
		if err := os.WriteFile(out, encoded, 0600); err != nil {
			t.Fatal(err)
		}
	}
}
