package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"reflect"
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
		func(d *pdDiagnostics) { d.Version = 3 },
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

func TestPDDiagnosticsResetRetentionSchema(t *testing.T) {
	fixture := diagnosticFixture()
	fixture.Version = 2
	reset := pdDiagnosticRecord{Count: 1, Text: "alignment evidence: reason=tx_dispatch mono_ns=123"}
	fixture.AlignmentReset = &reset
	fixture.Alignment.Text = "alignment evidence: reason=acquisition mono_ns=456"
	encoded, _ := json.Marshal(fixture)
	parsed, err := parsePDDiagnostics(encoded)
	if err != nil || !reflect.DeepEqual(parsed, &fixture) {
		t.Fatalf("reset and subsequent acquisition not preserved: %+v %v", parsed, err)
	}
	for _, change := range []func(*pdDiagnostics){
		func(d *pdDiagnostics) { d.AlignmentReset = nil },
		func(d *pdDiagnostics) { d.Version = 1 },
		func(d *pdDiagnostics) { d.AlignmentReset.Count = 3 },
		func(d *pdDiagnostics) { d.AlignmentReset.Count = 0 },
		func(d *pdDiagnostics) { d.AlignmentReset.Text = "alignment evidence: reason=acquisition mono_ns=1" },
		func(d *pdDiagnostics) { d.AlignmentReset.Text += "\n" },
		func(d *pdDiagnostics) { d.AlignmentReset.Text += "\"" },
		func(d *pdDiagnostics) { d.AlignmentReset.Truncated = true },
	} {
		bad := fixture
		r := reset
		bad.AlignmentReset = &r
		change(&bad)
		raw, _ := json.Marshal(bad)
		if _, err := parsePDDiagnostics(raw); err == nil {
			t.Fatalf("invalid reset accepted: %s", raw)
		}
	}
	for _, raw := range []string{
		strings.Replace(string(encoded), `"alignment_reset":{`, `"alignment_reset":null,"unknown":{`, 1),
		strings.Replace(string(encoded), `"alignment_reset":{`, `"alignment_reset":{},"alignment_reset":{`, 1),
	} {
		if _, err := parsePDDiagnostics([]byte(raw)); err == nil {
			t.Fatal("malformed reset schema accepted")
		}
	}
	empty := pdDiagnosticRecord{}
	fixture.AlignmentReset = &empty
	raw, _ := json.Marshal(fixture)
	if _, err := parsePDDiagnostics(raw); err != nil {
		t.Fatal("empty reset on a new process rejected", err)
	}
}

func TestPDDiagnosticsStatusProjection(t *testing.T) {
	for _, version := range []uint32{1, 2} {
		t.Run(fmt.Sprint(version), func(t *testing.T) { checkPDDiagnosticsStatusProjection(t, version) })
	}
}

func checkPDDiagnosticsStatusProjection(t *testing.T, version uint32) {
	t.Helper()
	var fields map[string]any
	if err := json.Unmarshal(pdFixture(t, "stall_bypassed"), &fields); err != nil {
		t.Fatal(err)
	}
	d := diagnosticFixture()
	d.Version = version
	if version == 2 {
		r := pdDiagnosticRecord{Count: 1, Text: "alignment evidence: reason=tx_dispatch mono_ns=123"}
		d.AlignmentReset = &r
	}
	fields["diagnostics"] = d
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
	if reply.Code != http.StatusOK || json.Unmarshal(reply.Body.Bytes(), &got) != nil || !reflect.DeepEqual(got.Diagnostics, status.Diagnostics) {
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
