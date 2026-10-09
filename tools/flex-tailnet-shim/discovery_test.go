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
	ag, ok := parseAnnouncement([]byte(agAnnouncement), netip.MustParseAddr("192.168.50.103"), 9007)
	if !ok || ag.Kind != "Antenna Genius" || ag.IP != "192.168.50.103" || ag.Port != 9007 ||
		ag.Name != "Antenna_Genius" || ag.Serial != "74-32-B6" {
		t.Fatalf("AG parsed as %+v (ok=%v)", ag, ok)
	}
	tg, ok := parseAnnouncement([]byte(tgxlAnnouncement+"\x00\x00"), netip.MustParseAddr("192.168.50.101"), 9010)
	if !ok || tg.Kind != "Tuner Genius XL" || tg.IP != "192.168.50.101" || tg.Port != 9010 ||
		tg.Name != "KK7GWY_-_TGXL" {
		t.Fatalf("TGXL parsed as %+v (ok=%v)", tg, ok)
	}
	// A LAN host can't have another address shared by naming it in ip=:
	// the device is where the announcement came from.
	forged, ok := parseAnnouncement([]byte(agAnnouncement), netip.MustParseAddr("192.168.50.250"), 9007)
	if !ok || forged.IP != "192.168.50.250" {
		t.Fatalf("an announcement's ip= overrode its sender: %+v (ok=%v)", forged, ok)
	}
	for _, bad := range []struct{ text, src string }{
		{"", "192.168.1.5"},
		{"Hello world", "192.168.1.5"},
		{"AG ip=192.168.1.5 port=9007", "8.8.4.4"},      // public sender, private ip=
		{"AG ip=192.168.1.5 port=9007", "172.30.1.5"},   // container network
		{"Unknown ip=192.168.1.5 v=1.0", "192.168.1.5"}, // not a 4O3A device
	} {
		if d, ok := parseAnnouncement([]byte(bad.text), netip.MustParseAddr(bad.src), 9007); ok {
			t.Errorf("%q from %s accepted as %+v", bad.text, bad.src, d)
		}
	}
}

// Announcements can't grow the advertised routes without bound.
func TestDiscoveredDevicesAreCapped(t *testing.T) {
	d := &Discovery{}
	for i := 0; i < maxDevices+10; i++ {
		d.note(Device{Kind: "Antenna Genius", IP: fmt.Sprintf("192.168.7.%d", i+1), LastSeen: time.Now()})
	}
	if got := len(d.Devices()); got != maxDevices {
		t.Fatalf("%d devices listed, want at most %d", got, maxDevices)
	}
	// A device already listed still refreshes.
	d.note(Device{Kind: "Antenna Genius", IP: "192.168.7.1", Name: "renamed", LastSeen: time.Now()})
	if d.Devices()[0].Name != "renamed" {
		t.Fatal("a listed device's announcement was ignored at the cap")
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
		HostUDP: func(netip.Addr) (*net.UDPConn, error) {
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
	n.authorize = func(net.Addr) (string, bool) { return "ops@example.com on laptop", true }
	n.localAddrs = func() []netip.Addr { return []netip.Addr{netip.MustParseAddr("192.0.2.1")} }

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

// A tailnet peer the allowlist refuses never reaches a station device, even
// though the tailnet's ACLs routed its flow here.
func TestSubnetFlowsFromRefusedPeersNeverReachTheDevice(t *testing.T) {
	dev, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer dev.Close()
	accepted := make(chan struct{}, 1)
	go func() {
		if c, err := dev.Accept(); err == nil {
			accepted <- struct{}{}
			c.Close()
		}
	}()
	devAP := netip.MustParseAddrPort(dev.Addr().String())
	n := &Node{advertised: []netip.Prefix{netip.PrefixFrom(devAP.Addr(), 32)}}
	src := netip.MustParseAddrPort("100.64.0.9:40000")
	var asked net.Addr
	n.authorize = func(a net.Addr) (string, bool) { asked = a; return "guest on phone", false }

	for _, tc := range []struct {
		name string
		node *Node
	}{{"refused by the allowlist", n}, {"node not running", &Node{advertised: n.advertised}}} {
		h, intercept := tc.node.subnetTCP(src, devAP)
		if h == nil || !intercept {
			t.Fatalf("%s: a flow to an advertised route must be intercepted, not left to tsnet", tc.name)
		}
		client, server := net.Pipe()
		done := make(chan struct{})
		go func() { h(server); close(done) }()
		client.SetReadDeadline(time.Now().Add(3 * time.Second))
		if k, err := client.Read(make([]byte, 8)); err == nil {
			t.Fatalf("%s: the refused caller read %d bytes", tc.name, k)
		}
		client.Close()
		<-done
	}
	if got := n.approvedRoutes(); len(got) != 0 {
		t.Fatalf("a refused flow marked routes in use: %v", got)
	}
	if asked == nil || asked.String() != src.String() {
		t.Fatalf("the allowlist was asked about %v, want the flow's source %v", asked, src)
	}
	select {
	case <-accepted:
		t.Fatal("a refused caller's flow reached the station device")
	case <-time.After(200 * time.Millisecond):
	}
}

// A route wide enough to contain the radio's own address never splices into
// the radio itself, even for an allowed peer.
func TestSubnetFlowsNeverReachTheRadioItself(t *testing.T) {
	radio, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer radio.Close()
	accepted := make(chan struct{}, 1)
	go func() {
		if c, err := radio.Accept(); err == nil {
			accepted <- struct{}{}
			c.Close()
		}
	}()
	radioAP := netip.MustParseAddrPort(radio.Addr().String())
	route := netip.MustParsePrefix("127.0.0.0/24")
	for _, tc := range []struct {
		name      string
		radioAddr string
		local     []netip.Addr
	}{
		{"one of the host's addresses", "172.30.1.1", []netip.Addr{radioAP.Addr()}},
		{"RadioAddr", radioAP.Addr().String(), []netip.Addr{netip.MustParseAddr("192.0.2.1")}},
	} {
		n := &Node{advertised: []netip.Prefix{route}, RadioAddr: tc.radioAddr}
		n.authorize = func(net.Addr) (string, bool) { return "ops@example.com on laptop", true }
		local := tc.local
		n.localAddrs = func() []netip.Addr { return local }

		h, intercept := n.subnetTCP(netip.MustParseAddrPort("100.64.0.9:40000"), radioAP)
		if h == nil || !intercept {
			t.Fatalf("%s: a flow to an advertised route must be intercepted, not left to tsnet", tc.name)
		}
		client, server := net.Pipe()
		done := make(chan struct{})
		go func() { h(server); close(done) }()
		client.SetReadDeadline(time.Now().Add(3 * time.Second))
		client.Read(make([]byte, 8))
		client.Close()
		<-done
		select {
		case <-accepted:
			t.Fatalf("%s: a station-device flow reached the radio", tc.name)
		case <-time.After(200 * time.Millisecond):
		}
		if got := n.approvedRoutes(); len(got) != 0 {
			t.Fatalf("%s: a refused flow marked routes in use: %v", tc.name, got)
		}
	}
}
