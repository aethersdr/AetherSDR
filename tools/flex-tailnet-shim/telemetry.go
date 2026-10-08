package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/netip"
	"os"
	"sort"
	"strings"
	"sync"
	"time"

	"tailscale.com/client/local"
	"tailscale.com/tailcfg"
)

// Link telemetry for AetherSDR's Network Diagnostics window. Served on the
// tailnet only (TelemetryPort), and each caller sees only its own sessions:
// the shim identifies the caller with WhoIs and applies the same allowlist
// as the relay.
const TelemetryPort = 48993

// ── VITA-49 loss before the tunnel ───────────────────────────────────────

// vitaStats counts packets and sequence gaps per stream as datagrams leave
// the radio, before the tunnel. Gaps here are radio-side; gaps the client
// sees but this doesn't happened in the tunnel.
type vitaStats struct {
	mu      sync.Mutex
	streams map[uint32]*streamGaps
}

type streamGaps struct {
	packets, gaps, breaks uint64
	last                  int // last 4-bit packet count, -1 before the first
}

// note records one datagram. VITA-49 word 0 carries the packet type (bits
// 31-28) and a 4-bit packet count (bits 19-16); types 1, 3, 4 and 5 carry a
// stream ID in word 1.
func (v *vitaStats) note(b []byte) {
	if len(b) < 8 {
		return
	}
	w0 := binary.BigEndian.Uint32(b[0:4])
	switch w0 >> 28 {
	case 1, 3, 4, 5:
	default:
		return
	}
	id := binary.BigEndian.Uint32(b[4:8])
	count := int((w0 >> 16) & 0xF)
	v.mu.Lock()
	defer v.mu.Unlock()
	if v.streams == nil {
		v.streams = map[uint32]*streamGaps{}
	}
	s := v.streams[id]
	if s == nil {
		if len(v.streams) >= 64 {
			return // bounded: a radio session has a handful of streams
		}
		s = &streamGaps{last: -1}
		v.streams[id] = s
	}
	if s.last >= 0 {
		if count == s.last {
			return // a repeat, not 15 lost packets
		}
		if missed := uint64((count - s.last - 1) & 0xF); missed > 0 {
			s.gaps += missed
			s.breaks++
		}
	}
	s.last = count
	s.packets++
}

// StreamInfo is one stream's counters before the tunnel. Gaps counts missed
// packets; Breaks counts sequence discontinuities, which is the unit
// AetherSDR's own per-stream error counter uses, so the two compare directly.
type StreamInfo struct {
	StreamID string `json:"stream_id"`
	Packets  uint64 `json:"packets"`
	Gaps     uint64 `json:"gaps"`
	Breaks   uint64 `json:"breaks"`
}

func (v *vitaStats) snapshot() []StreamInfo {
	v.mu.Lock()
	defer v.mu.Unlock()
	out := make([]StreamInfo, 0, len(v.streams))
	for id, s := range v.streams {
		out = append(out, StreamInfo{StreamID: fmt.Sprintf("0x%08x", id), Packets: s.packets, Gaps: s.gaps, Breaks: s.breaks})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].StreamID < out[j].StreamID })
	return out
}

// ── Peer path, RTT and throughput ────────────────────────────────────────

type peerTelemetry struct {
	path        string // "direct", "peer-relay", "relay" or "unknown"
	endpoint    string // the peer's direct address, when direct
	relay       string // DERP region code
	pathChanges int
	pathSince   time.Time
	rxBytes     int64
	txBytes     int64
	rxKbps      float64 // from the client, as the radio's node sees it
	txKbps      float64 // to the client
	lastSample  time.Time
	handshake   time.Time
	rttMs       float64 // -1 until a ping succeeds
	rttVia      string
	rttAt       time.Time
}

type linkTelemetry struct {
	mu     sync.Mutex
	peers  map[netip.Addr]*peerTelemetry
	cpuPct float64
	rssKB  int64
}

func pathOf(curAddr, peerRelay, relay string) string {
	switch {
	case curAddr != "":
		return "direct"
	case peerRelay != "":
		return "peer-relay"
	case relay != "":
		return "relay"
	}
	return "unknown"
}

// sample refreshes path, throughput and handshake for every peer, then pings
// the peers that have relay sessions. Called every 2 s while the node runs.
func (t *linkTelemetry) sample(ctx context.Context, lc *local.Client, sessionPeers []netip.Addr, ping bool) {
	st, err := lc.Status(ctx)
	if err != nil {
		return
	}
	now := time.Now()
	t.mu.Lock()
	if t.peers == nil {
		t.peers = map[netip.Addr]*peerTelemetry{}
	}
	for _, p := range st.Peer {
		for _, ip := range p.TailscaleIPs {
			pt := t.peers[ip]
			if pt == nil {
				pt = &peerTelemetry{rttMs: -1, pathSince: now}
				t.peers[ip] = pt
			}
			path := pathOf(p.CurAddr, p.PeerRelay, p.Relay)
			if pt.path != "" && pt.path != path {
				pt.pathChanges++
				pt.pathSince = now
			}
			pt.path, pt.endpoint, pt.relay = path, p.CurAddr, p.Relay
			if !pt.lastSample.IsZero() {
				dt := now.Sub(pt.lastSample).Seconds()
				if dt > 0 {
					pt.rxKbps = float64(max(p.RxBytes-pt.rxBytes, 0)) * 8 / 1000 / dt
					pt.txKbps = float64(max(p.TxBytes-pt.txBytes, 0)) * 8 / 1000 / dt
				}
			}
			pt.rxBytes, pt.txBytes, pt.lastSample, pt.handshake = p.RxBytes, p.TxBytes, now, p.LastHandshake
		}
	}
	t.mu.Unlock()

	if !ping {
		return
	}
	for _, ip := range sessionPeers {
		pctx, cancel := context.WithTimeout(ctx, 3*time.Second)
		res, err := lc.Ping(pctx, ip, tailcfg.PingDisco)
		cancel()
		t.mu.Lock()
		if pt := t.peers[ip]; pt != nil {
			if err == nil && res != nil && res.Err == "" {
				pt.rttMs = res.LatencySeconds * 1000
				switch {
				case res.Endpoint != "":
					pt.rttVia = "direct"
				case res.PeerRelay != "":
					pt.rttVia = "peer relay"
				case res.DERPRegionCode != "":
					pt.rttVia = "DERP(" + res.DERPRegionCode + ")"
				}
				pt.rttAt = time.Now()
			}
		}
		t.mu.Unlock()
	}
}

// sampleSelf records the shim's own CPU share and resident memory.
func (t *linkTelemetry) sampleSelf(prevTicks *int64, prevAt *time.Time) {
	b, err := os.ReadFile("/proc/self/stat")
	if err != nil {
		return
	}
	f := strings.Fields(string(b))
	if len(f) < 15 {
		return
	}
	var ut, stt int64
	fmt.Sscan(f[13], &ut)
	fmt.Sscan(f[14], &stt)
	ticks := ut + stt
	now := time.Now()
	var rss int64
	if s, err := os.ReadFile("/proc/self/status"); err == nil {
		for _, l := range strings.Split(string(s), "\n") {
			if strings.HasPrefix(l, "VmRSS:") {
				fmt.Sscan(strings.TrimSpace(strings.TrimSuffix(strings.TrimPrefix(l, "VmRSS:"), "kB")), &rss)
			}
		}
	}
	t.mu.Lock()
	if !prevAt.IsZero() {
		// USER_HZ is 100 on Linux; percent of one core.
		t.cpuPct = float64(ticks-*prevTicks) / 100 / now.Sub(*prevAt).Seconds() * 100
	}
	t.rssKB = rss
	t.mu.Unlock()
	*prevTicks, *prevAt = ticks, now
}

// ── The caller-only endpoint ─────────────────────────────────────────────

// SessionReport is what GET /v1/session returns to one tailnet caller.
type SessionReport struct {
	Version      string        `json:"version"`
	Path         string        `json:"path"`
	Endpoint     string        `json:"endpoint"`
	Relay        string        `json:"relay"`
	RTTMs        float64       `json:"rtt_ms"` // -1 when unknown
	RTTVia       string        `json:"rtt_via"`
	RTTAgeS      float64       `json:"rtt_age_s"`
	PathChanges  int           `json:"path_changes"`
	PathSinceS   float64       `json:"path_since_s"`
	ToClientKbps float64       `json:"to_client_kbps"`
	FromClientKb float64       `json:"from_client_kbps"`
	HandshakeS   float64       `json:"last_handshake_s"`
	ShimCPUPct   float64       `json:"shim_cpu_pct"`
	ShimRSSKB    int64         `json:"shim_rss_kb"`
	MTUClamp     int           `json:"mtu_clamp"`
	Sessions     []SessionInfo `json:"sessions"`
}

func (n *Node) sessionReport(caller netip.Addr) SessionReport {
	rep := SessionReport{Version: shimVersion, RTTMs: -1, Sessions: []SessionInfo{}, MTUClamp: n.MaxMTU}
	n.mu.Lock()
	relay := n.relay
	n.mu.Unlock()
	if relay != nil {
		for _, s := range relay.Sessions() {
			if s.clientIP == caller {
				rep.Sessions = append(rep.Sessions, s)
			}
		}
	}
	t := &n.telemetry
	t.mu.Lock()
	defer t.mu.Unlock()
	rep.ShimCPUPct, rep.ShimRSSKB = t.cpuPct, t.rssKB
	if pt := t.peers[caller]; pt != nil {
		now := time.Now()
		rep.Path, rep.Endpoint, rep.Relay = pt.path, pt.endpoint, pt.relay
		rep.PathChanges, rep.PathSinceS = pt.pathChanges, now.Sub(pt.pathSince).Seconds()
		rep.ToClientKbps, rep.FromClientKb = pt.txKbps, pt.rxKbps
		if !pt.handshake.IsZero() {
			rep.HandshakeS = now.Sub(pt.handshake).Seconds()
		}
		if pt.rttMs >= 0 {
			rep.RTTMs, rep.RTTVia, rep.RTTAgeS = pt.rttMs, pt.rttVia, now.Sub(pt.rttAt).Seconds()
		}
	}
	return rep
}

// telemetryHandler serves GET /v1/session to tailnet callers. A caller the
// allowlist refuses gets 403; everyone else sees only their own sessions.
func (n *Node) telemetryHandler(authorize func(net.Addr) (string, bool)) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /v1/session", func(w http.ResponseWriter, r *http.Request) {
		ap, err := netip.ParseAddrPort(r.RemoteAddr)
		if err != nil {
			http.Error(w, "unknown caller", http.StatusForbidden)
			return
		}
		if _, ok := authorize(net.TCPAddrFromAddrPort(ap)); !ok {
			http.Error(w, "not allowed", http.StatusForbidden)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		_ = json.NewEncoder(w).Encode(n.sessionReport(ap.Addr().Unmap()))
	})
	return mux
}

// runTelemetry samples every 2 s (pinging session peers every other tick)
// until ctx ends.
func (n *Node) runTelemetry(ctx context.Context, lc *local.Client) {
	tick := time.NewTicker(2 * time.Second)
	defer tick.Stop()
	var prevTicks int64
	var prevAt time.Time
	for i := 0; ; i++ {
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
		}
		var peers []netip.Addr
		n.mu.Lock()
		relay := n.relay
		n.mu.Unlock()
		if relay != nil {
			seen := map[netip.Addr]bool{}
			for _, s := range relay.Sessions() {
				if !seen[s.clientIP] {
					seen[s.clientIP] = true
					peers = append(peers, s.clientIP)
				}
			}
		}
		n.telemetry.sample(ctx, lc, peers, i%2 == 0)
		n.telemetry.sampleSelf(&prevTicks, &prevAt)
	}
}
