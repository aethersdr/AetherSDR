package main

import (
	"encoding/binary"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"testing"
)

// vitaPacket builds a minimal VITA-49 header: type 3 (extension data with
// stream ID), the 4-bit packet count, and the stream ID.
func vitaPacket(count int, stream uint32) []byte {
	b := make([]byte, 28)
	binary.BigEndian.PutUint32(b[0:4], 3<<28|uint32(count&0xF)<<16|7)
	binary.BigEndian.PutUint32(b[4:8], stream)
	return b
}

func TestVitaGapCounting(t *testing.T) {
	var v vitaStats
	// 0..15 then wrap to 0, 1: no gaps across the 4-bit wrap.
	for i := 0; i < 18; i++ {
		v.note(vitaPacket(i, 0x40000000))
	}
	// The last count was 1; jumping to 5 skips 2, 3 and 4: three gaps.
	v.note(vitaPacket(5, 0x40000000))
	// A repeated count is a break (as AetherSDR counts it), not 15 lost packets.
	v.note(vitaPacket(5, 0x40000000))
	// A second stream, and a packet type without a stream ID (ignored).
	v.note(vitaPacket(9, 0x42000000))
	noSID := vitaPacket(0, 0)
	binary.BigEndian.PutUint32(noSID[0:4], 0<<28)
	v.note(noSID)
	v.note([]byte{1, 2, 3}) // runt, ignored

	got := v.snapshot()
	if len(got) != 2 {
		t.Fatalf("streams %+v, want two", got)
	}
	if got[0].StreamID != "0x40000000" || got[0].Packets != 20 || got[0].Gaps != 3 || got[0].Breaks != 2 {
		t.Fatalf("pan stream %+v, want 20 packets, 3 gaps, 2 breaks", got[0])
	}
	if got[1].Packets != 1 || got[1].Gaps != 0 {
		t.Fatalf("second stream %+v", got[1])
	}
}

func TestSessionReportShowsOnlyTheCallersSessions(t *testing.T) {
	mine := netip.MustParseAddr("100.64.0.5")
	theirs := netip.MustParseAddr("100.64.0.9")
	r := &Relay{}
	r.init()
	for i, ip := range []netip.Addr{mine, theirs, mine} {
		s := &Session{id: uint64(i + 1), relay: r, clientIP: ip, peer: "node"}
		s.vita.note(vitaPacket(0, 0x40000000))
		r.sessions[s.id] = s
	}
	n := &Node{MaxMTU: 1200, relay: r}
	n.telemetry.peers = map[netip.Addr]*peerTelemetry{
		mine:   {path: "relay", relay: "sea", rttMs: 48.5, rttVia: "DERP(sea)"},
		theirs: {path: "direct", endpoint: "203.0.113.7:41641", rttMs: 9},
	}
	rep := n.sessionReport(mine)
	if len(rep.Sessions) != 2 {
		t.Fatalf("caller got %d sessions, want its own 2", len(rep.Sessions))
	}
	if rep.Path != "relay" || rep.Relay != "sea" || rep.RTTMs != 48.5 || rep.MTUClamp != 1200 {
		t.Fatalf("report %+v", rep)
	}
	if len(rep.Sessions[0].Streams) != 1 {
		t.Fatalf("streams not reported: %+v", rep.Sessions[0])
	}

	allow := func(a net.Addr) (string, bool) {
		ap := netip.MustParseAddrPort(a.String())
		return "x", ap.Addr() == mine
	}
	h := n.telemetryHandler(allow)
	call := func(remote string) *httptest.ResponseRecorder {
		req := httptest.NewRequest("GET", "/v1/session", nil)
		req.RemoteAddr = remote
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, req)
		return rec
	}
	if rec := call("100.64.0.9:5000"); rec.Code != http.StatusForbidden {
		t.Fatalf("refused caller got %d", rec.Code)
	}
	rec := call("100.64.0.5:5000")
	var got SessionReport
	if rec.Code != http.StatusOK || json.Unmarshal(rec.Body.Bytes(), &got) != nil || len(got.Sessions) != 2 {
		t.Fatalf("allowed caller: %d %s", rec.Code, rec.Body.String())
	}
	if got.Endpoint == "203.0.113.7:41641" {
		t.Fatal("another peer's endpoint leaked")
	}
}

// The shim's counters must match AetherSDR's PanadapterStream accounting
// (every datagram is a packet; any count but last+1 is a break), or Network
// Diagnostics reports loss that never happened. 0,1,1,2 reaches the client
// unchanged as 4 packets and 1 break, so the radio side must say the same.
func TestVitaCountsMatchTheClientsAccounting(t *testing.T) {
	var v vitaStats
	for _, c := range []int{0, 1, 1, 2} {
		v.note(vitaPacket(c, 0x40000000))
	}
	got := v.snapshot()[0]
	if got.Packets != 4 || got.Breaks != 1 || got.Gaps != 0 {
		t.Fatalf("0,1,1,2 counted %+v; AetherSDR counts 4 packets, 1 break", got)
	}
}
