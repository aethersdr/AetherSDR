package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// A radio's announcement payload: a real field set, with lookalike keys
// (inuse_ip, gui_client_ips) that must keep the LAN addresses they name.
const announcePayload = "discovery_protocol_version=3.0.0.2 model=FLEX-6500 serial=2213-3052-6500-9487 " +
	"version=4.2.20.41343 nickname=K5PTB callsign=K5PTB ip=192.168.8.122 port=4992 status=Available " +
	"inuse_ip=192.168.8.50 inuse_host=shack-pc max_licensed_version=v3 gui_client_ips=192.168.8.50 " +
	"gui_client_hosts=shack-pc gui_client_programs=SmartSDR-Win"

// vitaAnnouncement wraps payload as FlexRadio sends it: extension data with
// a stream ID, class ID (Flex OUI, discovery class), integer and fractional
// timestamps, the payload padded with NULs to a word, and an optional
// trailer.
func vitaAnnouncement(payload string, trailer bool) []byte {
	b := make([]byte, 28)
	b[0] = 0x38 // type 3, class ID present
	if trailer {
		b[0] |= 0x04
	}
	b[1] = 0x50 // TSI 1, TSF 1, packet count 0
	binary.BigEndian.PutUint32(b[4:], 0x00000800)
	binary.BigEndian.PutUint32(b[8:], flexOUI)
	binary.BigEndian.PutUint32(b[12:], 0x534CFFFF)
	binary.BigEndian.PutUint32(b[16:], 0x6543210F)
	binary.BigEndian.PutUint64(b[20:], 0x0102030405060708)
	b = append(b, payload...)
	b = append(b, 0) // the radio's NUL terminator
	for len(b)%4 != 0 {
		b = append(b, 0)
	}
	if trailer {
		b = append(b, 0xDE, 0xAD, 0xBE, 0xEF)
	}
	binary.BigEndian.PutUint16(b[2:], uint16(len(b)/4))
	return b
}

func TestRewriteAnnouncementVITA(t *testing.T) {
	want := strings.Replace(announcePayload, " ip=192.168.8.122 ", " ip=100.64.0.1 ", 1)
	for _, trailer := range []bool{false, true} {
		in := vitaAnnouncement(announcePayload, trailer)
		orig := append([]byte(nil), in...)
		out, named, ok := rewriteAnnouncement(in, netip.MustParseAddr("100.64.0.1"))
		if !ok {
			t.Fatalf("trailer=%v: refused", trailer)
		}
		if !bytes.Equal(in, orig) {
			t.Fatalf("trailer=%v: the radio's packet was modified", trailer)
		}
		if named != netip.MustParseAddr("192.168.8.122") {
			t.Errorf("trailer=%v: named %v", trailer, named)
		}
		if len(out)%4 != 0 || int(binary.BigEndian.Uint16(out[2:]))*4 != len(out) {
			t.Fatalf("trailer=%v: size field %d words for %d bytes", trailer, binary.BigEndian.Uint16(out[2:]), len(out))
		}
		if !bytes.Equal(out[:2], in[:2]) || !bytes.Equal(out[4:28], in[4:28]) {
			t.Errorf("trailer=%v: header changed beyond the size", trailer)
		}
		end := len(out)
		if trailer {
			end -= 4
			if !bytes.Equal(out[end:], []byte{0xDE, 0xAD, 0xBE, 0xEF}) {
				t.Errorf("trailer lost: % x", out[end:])
			}
		}
		if got := string(bytes.TrimRight(out[28:end], "\x00")); got != want {
			t.Errorf("trailer=%v: payload\n got %q\nwant %q", trailer, got, want)
		}
	}
}

func TestRewriteAnnouncementText(t *testing.T) {
	in := "name=flex-sim model=FLEX-6600 serial=EMULATE01 ip=10.0.0.5 port=4992 status=Available"
	out, named, ok := rewriteAnnouncement([]byte(in), netip.MustParseAddr("100.101.102.103"))
	want := "name=flex-sim model=FLEX-6600 serial=EMULATE01 ip=100.101.102.103 port=4992 status=Available"
	if !ok || string(out) != want || named != netip.MustParseAddr("10.0.0.5") {
		t.Fatalf("got %q %v %v", out, named, ok)
	}
	// ip= first in the packet is still the radio's address.
	out, _, ok = rewriteAnnouncement([]byte("ip=10.0.0.5 serial=X"), netip.MustParseAddr("100.64.0.9"))
	if !ok || string(out) != "ip=100.64.0.9 serial=X" {
		t.Fatalf("leading ip=: got %q %v", out, ok)
	}
}

func TestRewriteAnnouncementRefuses(t *testing.T) {
	self := netip.MustParseAddr("100.64.0.1")
	for _, in := range [][]byte{
		[]byte("model=FLEX-6500 ip=192.168.8.122 port=4992"),          // no serial
		[]byte("model=FLEX-6500 serial=1234 port=4992"),               // no ip
		[]byte("serial= ip=192.168.8.122"),                            // empty serial
		[]byte("model=X serial=1 inuse_ip=192.168.8.50"),              // only a lookalike key
		vitaAnnouncement("model=FLEX-6500 serial=1 port=4992", false), // VITA, no ip
		{},
	} {
		if out, _, ok := rewriteAnnouncement(in, self); ok {
			t.Errorf("%q: accepted as %q", in, out)
		}
	}
}

func TestSelectTargets(t *testing.T) {
	station := []netip.Prefix{netip.MustParsePrefix("192.168.8.0/24")}
	peers := func() []tailPeer {
		return []tailPeer{
			{ID: "nRemote", Name: "laptop", Online: true, IP: netip.MustParseAddr("100.64.0.2"), Direct: netip.MustParseAddr("203.0.113.9")},
			{ID: "nRelayed", Name: "desktop", Online: true, IP: netip.MustParseAddr("100.64.0.3")},
			{ID: "nAway", Name: "tablet", Online: false, IP: netip.MustParseAddr("100.64.0.4")},
			{ID: "nShack", Name: "shack-pc", Online: true, IP: netip.MustParseAddr("100.64.0.5"), Direct: netip.MustParseAddr("192.168.8.50")},
			{ID: "nV6", Name: "v6only", Online: true},
		}
	}
	reasons := func(r []AnnouncePeer) string {
		var s []string
		for _, p := range r {
			s = append(s, fmt.Sprintf("%s:%v/%s", p.Name, p.Announced, p.Skipped))
		}
		return strings.Join(s, " ")
	}
	addrs := func(a []netip.Addr) string { return fmt.Sprint(a) }

	targets, report := selectTargets(peers(), announceAll, nil, station, nil)
	if got := addrs(targets); got != "[100.64.0.3 100.64.0.2]" {
		t.Errorf("all: targets %s", got)
	}
	if got := reasons(report); got != "desktop:true/ laptop:true/ shack-pc:false/at the station tablet:false/offline v6only:false/no IPv4" {
		t.Errorf("all: report %s", got)
	}
	targets, report = selectTargets(peers(), announceOnly, []string{"nRemote", "nAway", "nShack"}, station, nil)
	if got := addrs(targets); got != "[100.64.0.2]" {
		t.Errorf("only: targets %s", got)
	}
	if got := reasons(report); got != "desktop:false/not chosen laptop:true/ shack-pc:false/at the station tablet:false/offline v6only:false/not chosen" {
		t.Errorf("only: report %s", got)
	}
	if targets, _ = selectTargets(peers(), announceOff, nil, station, nil); len(targets) != 0 {
		t.Errorf("off: targets %v", targets)
	}
	// Before the station network is known, nobody is taken to be at it.
	if targets, _ = selectTargets(peers(), announceAll, nil, nil, nil); len(targets) != 3 {
		t.Errorf("no station: targets %v", targets)
	}
	var many []tailPeer
	for i := range maxAnnounceTargets + 3 {
		many = append(many, tailPeer{ID: fmt.Sprintf("n%02d", i), Name: fmt.Sprintf("d%02d", i), Online: true,
			IP: netip.AddrFrom4([4]byte{100, 64, 1, byte(i)})})
	}
	targets, report = selectTargets(many, announceAll, nil, station, nil)
	if len(targets) != maxAnnounceTargets || report[len(report)-1].Skipped != "over the limit" {
		t.Errorf("limit: %d targets, last %+v", len(targets), report[len(report)-1])
	}
}

// A device with a discovery helper is served by the helper, even when the
// device choice leaves it out, and is not also sent a copy; "off" stops it.
func TestSelectTargetsHelper(t *testing.T) {
	peers := []tailPeer{
		{ID: "nWin", Name: "winpc", Online: true, IP: netip.MustParseAddr("100.64.0.7")},
		{ID: "nMac", Name: "mac", Online: true, IP: netip.MustParseAddr("100.64.0.8")},
	}
	helpers := map[netip.Addr]bool{netip.MustParseAddr("100.64.0.7"): true}
	for _, tc := range []struct {
		mode, wantTargets, wantWin string
	}{
		{announceAll, "[100.64.0.8]", "true/helper/"},
		{announceOnly, "[]", "true/helper/"},
		{announceOff, "[]", "false//off"},
	} {
		targets, report := selectTargets(peers, tc.mode, nil, nil, helpers)
		var win AnnouncePeer
		for _, r := range report {
			if r.ID == "nWin" {
				win = r
			}
		}
		if got := fmt.Sprint(targets); got != tc.wantTargets {
			t.Errorf("%s: targets %s, want %s", tc.mode, got, tc.wantTargets)
		}
		if got := fmt.Sprintf("%v/%s/%s", win.Announced, win.Via, win.Skipped); got != tc.wantWin {
			t.Errorf("%s: helper device %s, want %s", tc.mode, got, tc.wantWin)
		}
	}
}

type sentAnnouncement struct {
	b  []byte
	to netip.AddrPort
}

// TestAnnouncerForwards runs the announcer over a real UDP socket: only the
// radio's announcements are forwarded, rewritten, to the selected devices,
// never to one at the station, no faster than announceInterval, and nothing
// goes out once detached.
func TestAnnouncerForwards(t *testing.T) {
	pc, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer pc.Close()
	var fromRadio atomic.Bool
	a := &Announcer{
		IsRadio:    func(netip.Addr) bool { return fromRadio.Load() },
		Listen:     func() []net.PacketConn { return []net.PacketConn{pc} },
		Interfaces: func() []netip.Prefix { return []netip.Prefix{netip.MustParsePrefix("192.168.8.0/24")} },
	}
	a.Start()

	var mu sync.Mutex
	var sent []sentAnnouncement
	selected := make(chan struct{}, 8)
	self := netip.MustParseAddr("100.112.196.2")
	a.Attach(self, func(b []byte, to netip.AddrPort) error {
		mu.Lock()
		sent = append(sent, sentAnnouncement{append([]byte(nil), b...), to})
		mu.Unlock()
		return nil
	}, func(context.Context) ([]tailPeer, error) {
		defer func() { selected <- struct{}{} }()
		return []tailPeer{
			{ID: "nA", Name: "a", Online: true, IP: netip.MustParseAddr("100.64.0.2")},
			{ID: "nB", Name: "b", Online: true, IP: netip.MustParseAddr("100.64.0.3")},
			{ID: "nS", Name: "shack", Online: true, IP: netip.MustParseAddr("100.64.0.4"),
				Direct: netip.MustParseAddr("192.168.8.50")},
		}, nil
	}, func() (string, []string) { return announceAll, nil })
	<-selected
	waitFor(t, "device selection", func() bool { return len(a.Status().Devices) == 3 })

	sendTo := func() {
		c, err := net.Dial("udp4", pc.LocalAddr().String())
		if err != nil {
			t.Fatal(err)
		}
		defer c.Close()
		c.Write(vitaAnnouncement(announcePayload, false))
	}
	count := func() int {
		mu.Lock()
		defer mu.Unlock()
		return len(sent)
	}

	sendTo() // not the radio: ignored
	time.Sleep(100 * time.Millisecond)
	if n := count(); n != 0 || a.Status().Heard != "" {
		t.Fatalf("forwarded another sender's announcement: %d sent, heard %q", n, a.Status().Heard)
	}
	fromRadio.Store(true)
	// The first announcement places the station; the selection made before
	// it is redone, and nothing is sent until it has been, so the shack PC
	// is never sent one.
	sendTo()
	<-selected
	time.Sleep(50 * time.Millisecond)
	if n := count(); n != 0 {
		t.Fatalf("%d sent before the station was known", n)
	}
	sendTo()
	waitFor(t, "two copies sent", func() bool { return count() == 2 })
	sendTo() // within announceInterval: not forwarded
	time.Sleep(100 * time.Millisecond)
	if n := count(); n != 2 {
		t.Fatalf("%d sent after a second announcement within the interval, want 2", n)
	}
	mu.Lock()
	for i, s := range sent {
		wantTo := netip.AddrPortFrom(netip.MustParseAddr([]string{"100.64.0.2", "100.64.0.3"}[i]), announcePort)
		if s.to != wantTo {
			t.Errorf("sent to %v, want %v", s.to, wantTo)
		}
		if !bytes.Contains(s.b, []byte(" ip=100.112.196.2 ")) || bytes.Contains(s.b, []byte(" ip=192.168.8.122")) {
			t.Errorf("not rewritten: %q", s.b)
		}
	}
	mu.Unlock()
	if a.Status().Heard == "" {
		t.Error("status doesn't say the radio was heard")
	}

	a.Detach()
	time.Sleep(announceInterval)
	sendTo()
	time.Sleep(100 * time.Millisecond)
	if n := count(); n != 2 {
		t.Fatalf("%d sent after Detach, want 2", n)
	}
	if a.Status().Heard == "" || len(a.Status().Devices) != 0 {
		t.Errorf("detached status: %+v", a.Status())
	}
}

func TestAnnounceSettingAPI(t *testing.T) {
	const token = "test-admin-token"
	n := newTestNode(t, token)
	h := NewAPI(n)
	const lan = "192.168.1.10:5000"
	if rec := call(t, h, "PUT", "/v1/announce", lan, "", `{"announce":"off"}`); rec.Code != 401 {
		t.Fatalf("without token: got %d", rec.Code)
	}
	for _, body := range []string{
		`{"announce":"some"}`,
		`{"announce":""}`,
		`{"announce":"only","announce_to":["n1; rm"]}`,
		`{"announce":"only","extra":1}`,
	} {
		if rec := call(t, h, "PUT", "/v1/announce", lan, token, body); rec.Code != 400 {
			t.Errorf("%s: got %d, want 400", body, rec.Code)
		}
	}
	if st := n.Status(); st.Announce != announceAll || len(st.AnnounceTo) != 0 {
		t.Fatalf("default: %q %v", st.Announce, st.AnnounceTo)
	}
	rec := call(t, h, "PUT", "/v1/announce", lan, token, `{"announce":"only","announce_to":["nAbC123CNTRL"]}`)
	if rec.Code != 200 {
		t.Fatalf("with token: got %d %s", rec.Code, rec.Body.String())
	}
	var st Status
	if err := json.Unmarshal(rec.Body.Bytes(), &st); err != nil {
		t.Fatal(err)
	}
	if st.Announce != announceOnly || fmt.Sprint(st.AnnounceTo) != "[nAbC123CNTRL]" {
		t.Fatalf("reply: %q %v", st.Announce, st.AnnounceTo)
	}
	// The setting survives a restart.
	again := &Node{StateDir: n.StateDir}
	if err := again.load(); err != nil {
		t.Fatal(err)
	}
	if st := again.Status(); st.Announce != announceOnly || fmt.Sprint(st.AnnounceTo) != "[nAbC123CNTRL]" {
		t.Fatalf("after reload: %q %v", st.Announce, st.AnnounceTo)
	}
	// "all" is stored as the default, so an older shim reads it the same.
	if rec := call(t, h, "PUT", "/v1/announce", lan, token, `{"announce":"all"}`); rec.Code != 200 {
		t.Fatalf("all: got %d", rec.Code)
	}
	b, err := os.ReadFile(filepath.Join(n.StateDir, "provision.json"))
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(b), "announce") {
		t.Errorf("default stored explicitly: %s", b)
	}
}

// readFrame reads one helper frame, or fails after d.
func readFrame(c net.Conn, d time.Duration) ([]byte, error) {
	c.SetReadDeadline(time.Now().Add(d))
	var n [2]byte
	if _, err := io.ReadFull(c, n[:]); err != nil {
		return nil, err
	}
	b := make([]byte, binary.BigEndian.Uint16(n[:]))
	_, err := io.ReadFull(c, b)
	return b, err
}

// TestAnnouncerHelperFeed runs the helper feed over loopback TCP: an
// admitted helper gets each forwarded announcement, rewritten, and its
// device is not also sent a copy; a helper connecting later gets the last
// announcement at once; a refused one gets nothing; "off" sends nothing;
// and detaching closes the helper.
func TestAnnouncerHelperFeed(t *testing.T) {
	pc, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer pc.Close()
	ln, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	a := &Announcer{
		IsRadio: func(netip.Addr) bool { return true },
		Listen:  func() []net.PacketConn { return []net.PacketConn{pc} },
	}
	a.Start()
	var mu sync.Mutex
	var sent []netip.AddrPort
	mode := announceAll
	a.Attach(netip.MustParseAddr("100.112.196.2"), func(b []byte, to netip.AddrPort) error {
		mu.Lock()
		sent = append(sent, to)
		mu.Unlock()
		return nil
	}, func(context.Context) ([]tailPeer, error) {
		return []tailPeer{
			{ID: "nWin", Name: "winpc", Online: true, IP: netip.MustParseAddr("127.0.0.1")},
			{ID: "nMac", Name: "mac", Online: true, IP: netip.MustParseAddr("100.64.0.8")},
		}, nil
	}, func() (string, []string) {
		mu.Lock()
		defer mu.Unlock()
		return mode, nil
	})
	var admit atomic.Bool
	go a.ServeHelpers(ln, func(net.Addr) (string, bool) { return "winpc", admit.Load() })

	announce := func() {
		c, err := net.Dial("udp4", pc.LocalAddr().String())
		if err != nil {
			t.Fatal(err)
		}
		defer c.Close()
		c.Write(vitaAnnouncement(announcePayload, false))
	}
	dial := func() net.Conn {
		c, err := net.Dial("tcp4", ln.Addr().String())
		if err != nil {
			t.Fatal(err)
		}
		return c
	}
	viaHelper := func() bool {
		for _, d := range a.Status().Devices {
			if d.ID == "nWin" && d.Via == "helper" {
				return true
			}
		}
		return false
	}

	admit.Store(true)
	h := dial()
	defer h.Close()
	waitFor(t, "the helper's device to be served by it", viaHelper)
	announce() // places the station; not sent
	time.Sleep(100 * time.Millisecond)
	announce()
	b, err := readFrame(h, 2*time.Second)
	if err != nil {
		t.Fatalf("helper: %v", err)
	}
	if !bytes.Contains(b, []byte(" ip=100.112.196.2 ")) || binary.BigEndian.Uint32(b[8:])&0x00FFFFFF != flexOUI {
		t.Fatalf("helper got %q", b)
	}
	mu.Lock()
	if fmt.Sprint(sent) != "[100.64.0.8:4992]" {
		t.Errorf("copies sent to %v; the helper's device must not get one", sent)
	}
	mu.Unlock()

	late := dial()
	defer late.Close()
	if b2, err := readFrame(late, time.Second); err != nil || !bytes.Equal(b2, b) {
		t.Fatalf("a helper connecting later wasn't sent the last announcement: %v", err)
	}
	// A refused helper is closed with nothing sent, though there is an
	// announcement it would otherwise be sent at once.
	admit.Store(false)
	refused := dial()
	if _, err := readFrame(refused, time.Second); err != io.EOF {
		t.Fatalf("refused helper: %v, want closed with nothing sent", err)
	}
	refused.Close()

	mu.Lock()
	mode = announceOff
	mu.Unlock()
	time.Sleep(announceInterval)
	announce()
	if _, err := readFrame(h, 300*time.Millisecond); err == nil {
		t.Fatal("helper fed with announcements off")
	}

	a.Detach()
	h.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := h.Read(make([]byte, 64)); err != io.EOF {
		t.Fatalf("helper not closed on detach: %v", err)
	}
}
