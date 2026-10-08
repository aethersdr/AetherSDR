package main

import (
	"bufio"
	"fmt"
	"net"
	"net/netip"
	"strings"
	"testing"
	"time"
)

// Announcements captured from a real station LAN on 2026-10-07.
const (
	agAnnouncement   = "AG ip=192.168.50.103 port=9007 v=4.1.16 serial=74-32-B6 name=Antenna_Genius ports=2 antennas=8 mode=master uptime=2164621"
	tgxlAnnouncement = "TunerGenius ip=192.168.50.101 v=1.2.17 serial=251344-1 nickname=KK7GWY_-_TGXL"
)

func TestParseAnnouncement(t *testing.T) {
	src := netip.MustParseAddr("192.168.50.250")
	ag, ok := parseAnnouncement([]byte(agAnnouncement), src, 9007)
	if !ok || ag.Kind != "Antenna Genius" || ag.IP != "192.168.50.103" || ag.Port != 9007 ||
		ag.Name != "Antenna_Genius" || ag.Serial != "74-32-B6" {
		t.Fatalf("AG parsed as %+v (ok=%v)", ag, ok)
	}
	tg, ok := parseAnnouncement([]byte(tgxlAnnouncement+"\x00\x00"), src, 9010)
	if !ok || tg.Kind != "Tuner Genius XL" || tg.IP != "192.168.50.101" || tg.Port != 9010 ||
		tg.Name != "KK7GWY_-_TGXL" {
		t.Fatalf("TGXL parsed as %+v (ok=%v)", tg, ok)
	}
	for _, bad := range []string{
		"",
		"Hello world",
		"AG ip=8.8.8.8 port=9007",      // public address
		"AG ip=172.30.1.5 port=9007",   // container network
		"Unknown ip=192.168.1.5 v=1.0", // not a 4O3A device
	} {
		if d, ok := parseAnnouncement([]byte(bad), netip.MustParseAddr("8.8.4.4"), 9007); ok {
			t.Errorf("%q accepted as %+v", bad, d)
		}
	}
}

func TestParseRoute(t *testing.T) {
	good := map[string]string{
		"192.168.50.103":   "192.168.50.103/32",
		"192.168.50.96/28": "192.168.50.96/28",
		"10.1.2.3":         "10.1.2.3/32",
		"172.16.5.0/24":    "172.16.5.0/24",
	}
	for in, want := range good {
		p, err := parseRoute(in)
		if err != nil || p.String() != want {
			t.Errorf("%s: got %v %v, want %s", in, p, err, want)
		}
	}
	for _, bad := range []string{"0.0.0.0/0", "192.168.0.0/8", "8.8.8.8", "172.30.1.1", "172.30.0.0/16",
		"100.100.1.2", "::1", "fd00::/64", "nonsense", "10.0.0.0/12"} {
		if p, err := parseRoute(bad); err == nil {
			t.Errorf("%s accepted as %v", bad, p)
		}
	}
}

func TestEffectiveRoutesUnionDiscoveredAndManual(t *testing.T) {
	d := &Discovery{}
	d.note(Device{Kind: "Antenna Genius", IP: "192.168.50.103", LastSeen: time.Now()})
	d.note(Device{Kind: "Tuner Genius XL", IP: "192.168.50.101", LastSeen: time.Now()})
	n := &Node{Discovery: d}
	got := fmt.Sprint(n.effectiveRoutes(provisionConfig{Routes: []string{"192.168.50.120", "192.168.50.103"}}))
	if got != "[192.168.50.101/32 192.168.50.103/32 192.168.50.120/32]" {
		t.Fatalf("default sharing: %s", got)
	}
	off := false
	got = fmt.Sprint(n.effectiveRoutes(provisionConfig{Routes: []string{"192.168.50.120"}, ShareDiscovered: &off}))
	if got != "[192.168.50.120/32]" {
		t.Fatalf("sharing off: %s", got)
	}
}

func TestMTUClampInjectedAndReplySwallowed(t *testing.T) {
	fr := newFakeRadio(t)
	relay := &Relay{
		RadioAddr: "127.0.0.1",
		DialRadio: func() (net.Conn, error) { return net.Dial("tcp", fr.api.Addr().String()) },
		HostUDP: func() (*net.UDPConn, error) {
			return net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
		},
		ClientTX: loopUDP(t), ClientPrime: loopUDP(t), ClientOut: loopUDP(t),
		MaxMTU: 1200,
	}
	ln, _ := net.Listen("tcp", "127.0.0.1:0")
	defer ln.Close()
	go relay.Serve(ln)
	ctl, err := net.Dial("tcp", ln.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer ctl.Close()
	rd := bufio.NewReader(ctl)
	rd.ReadString('\n')
	rd.ReadString('\n')

	fmt.Fprint(ctl, "C1|client gui\n")
	var lines []string
	for len(lines) < 2 {
		select {
		case l := <-fr.lines:
			lines = append(lines, l)
		case <-time.After(2 * time.Second):
			t.Fatalf("radio saw only %v", lines)
		}
	}
	if lines[0] != "C1|client gui" || !strings.HasSuffix(lines[1], "|client set enforce_network_mtu=1 network_mtu=1200") {
		t.Fatalf("radio saw %v, want client gui then the shim's MTU clamp", lines)
	}
	// The fake radio answers `info` with R<seq>|0|5000; use it as a marker that
	// any reply to the injected command would already have arrived.
	fmt.Fprint(ctl, "C2|info\n")
	ctl.SetReadDeadline(time.Now().Add(2 * time.Second))
	l, err := rd.ReadString('\n')
	if err != nil || l != "R2|0|5000\n" {
		t.Fatalf("client saw %q (%v); the injected command's reply must not reach it", l, err)
	}
}

func TestSubnetFlowsAreForwardedAndMarkRoutesInUse(t *testing.T) {
	// A stand-in station device on loopback that greets each client.
	dev, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer dev.Close()
	go func() {
		for {
			c, err := dev.Accept()
			if err != nil {
				return
			}
			c.Write([]byte("AG hello"))
			c.Close()
		}
	}()
	devAP := netip.MustParseAddrPort(dev.Addr().String())
	route := netip.PrefixFrom(devAP.Addr(), 32)
	n := &Node{advertised: []netip.Prefix{route}}

	src := netip.MustParseAddrPort("100.64.0.9:40000")
	if h, intercept := n.subnetTCP(src, netip.MustParseAddrPort("192.0.2.7:9007")); h != nil || intercept {
		t.Fatal("a flow outside the advertised routes must be left alone")
	}
	h, intercept := n.subnetTCP(src, devAP)
	if h == nil || !intercept {
		t.Fatal("a flow to an advertised route must be handled")
	}
	client, server := net.Pipe()
	go h(server)
	client.SetReadDeadline(time.Now().Add(3 * time.Second))
	buf := make([]byte, 32)
	k, _ := client.Read(buf)
	client.Close()
	if string(buf[:k]) != "AG hello" {
		t.Fatalf("device greeting not spliced through: %q", buf[:k])
	}
	if got := n.approvedRoutes(); len(got) != 1 || got[0] != route.String() {
		t.Fatalf("a route that carried traffic must count as approved, got %v", got)
	}
}
