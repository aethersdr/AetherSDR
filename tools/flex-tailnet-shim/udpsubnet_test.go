package main

import (
	"net"
	"net/netip"
	"sync"
	"testing"
	"time"
)

// chanListener stands in for a tsnet UDP listener: each Conn pushed is one
// client flow.
type chanListener struct {
	ap    netip.AddrPort
	conns chan net.Conn
	once  sync.Once
	done  chan struct{}
}

func newChanListener(ap netip.AddrPort) *chanListener {
	return &chanListener{ap: ap, conns: make(chan net.Conn), done: make(chan struct{})}
}

func (l *chanListener) Accept() (net.Conn, error) {
	select {
	case c := <-l.conns:
		return c, nil
	case <-l.done:
		return nil, net.ErrClosed
	}
}
func (l *chanListener) Close() error   { l.once.Do(func() { close(l.done) }); return nil }
func (l *chanListener) Addr() net.Addr { return net.UDPAddrFromAddrPort(l.ap) }

func (l *chanListener) closed() bool {
	select {
	case <-l.done:
		return true
	default:
		return false
	}
}

func isTimeout(err error) bool {
	ne, ok := err.(net.Error)
	return ok && ne.Timeout()
}

// udpEcho is a stand-in HL2 on loopback that answers each datagram.
func udpEcho(t *testing.T) (netip.AddrPort, func() int) {
	t.Helper()
	pc, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { pc.Close() })
	var mu sync.Mutex
	got := 0
	go func() {
		buf := make([]byte, 2048)
		for {
			k, from, err := pc.ReadFromUDPAddrPort(buf)
			if err != nil {
				return
			}
			mu.Lock()
			got++
			mu.Unlock()
			pc.WriteToUDPAddrPort(append([]byte("HL2:"), buf[:k]...), from)
		}
	}()
	return netip.MustParseAddrPort(pc.LocalAddr().String()), func() int { mu.Lock(); defer mu.Unlock(); return got }
}

// hl2Node is a running node sharing one discovered HL2 at 192.168.50.120,
// whose LAN side is dev.
func hl2Node(t *testing.T, dev netip.AddrPort, allow bool) (*Node, map[netip.AddrPort]*chanListener) {
	t.Helper()
	disc := &Discovery{}
	hl2, _ := parseHL2Reply(hl2Reply(0x02), netip.MustParseAddrPort("192.168.50.120:1024"))
	hl2.LastSeen = time.Now()
	disc.note(hl2)
	listeners := map[netip.AddrPort]*chanListener{}
	n := &Node{Discovery: disc, advertised: []netip.Prefix{netip.MustParsePrefix("192.168.50.120/32")}}
	n.authorize = func(net.Addr) (string, bool) { return "ops@example.com on laptop", allow }
	n.localAddrs = func() []netip.Addr { return []netip.Addr{netip.MustParseAddr("192.168.50.254")} }
	n.listenRoutedUDP = func(ap netip.AddrPort) (net.Listener, error) {
		l := newChanListener(ap)
		listeners[ap] = l
		return l, nil
	}
	n.dialDeviceUDP = func(netip.AddrPort) (net.Conn, error) {
		return net.DialUDP("udp4", nil, net.UDPAddrFromAddrPort(dev))
	}
	return n, listeners
}

func TestSharedHL2UDPIsRelayedBothWays(t *testing.T) {
	dev, received := udpEcho(t)
	n, listeners := hl2Node(t, dev, true)
	n.syncUDP()
	l := listeners[netip.MustParseAddrPort("192.168.50.120:1024")]
	if l == nil || len(listeners) != 1 {
		t.Fatalf("listened on %v, want only 192.168.50.120:1024", listeners)
	}
	client, flow := net.Pipe()
	l.conns <- flow
	client.SetDeadline(time.Now().Add(3 * time.Second))
	for _, msg := range []string{"discover", "ep2 frame"} {
		if _, err := client.Write([]byte(msg)); err != nil {
			t.Fatal(err)
		}
		buf := make([]byte, 64)
		k, err := client.Read(buf)
		if err != nil || string(buf[:k]) != "HL2:"+msg {
			t.Fatalf("reply %q (%v), want %q", buf[:k], err, "HL2:"+msg)
		}
	}
	if got := n.approvedRoutes(); len(got) != 1 || got[0] != "192.168.50.120/32" {
		t.Fatalf("a route that carried UDP must count as approved, got %v", got)
	}
	if received() != 2 {
		t.Fatalf("device received %d datagrams, want 2", received())
	}

	// Withdrawing the route ends the flow and stops listening.
	n.mu.Lock()
	n.advertised = []netip.Prefix{}
	n.mu.Unlock()
	n.splices.unshare(n.shared)
	n.syncUDP()
	// Closed by the relay, not merely quiet until the deadline.
	if _, err := client.Read(make([]byte, 8)); err == nil || isTimeout(err) {
		t.Fatalf("a flow to an unshared HL2 kept running (%v)", err)
	}
	if !l.closed() {
		t.Fatal("still listening for an HL2 no longer shared")
	}
}

func TestUDPIsRelayedOnlyToSharedHL2s(t *testing.T) {
	dev, _ := udpEcho(t)
	n, listeners := hl2Node(t, dev, true)
	ag := Device{Kind: "Antenna Genius", IP: "192.168.50.103", Port: 9007, LastSeen: time.Now()}
	n.Discovery.note(ag)
	n.advertised = []netip.Prefix{netip.MustParsePrefix("192.168.50.103/32")} // the AG, not the HL2
	n.syncUDP()
	if len(listeners) != 0 {
		t.Fatalf("listened on %v: an unshared HL2 and a TCP device need no UDP", listeners)
	}
	// A route already shared by hand picks up an HL2 discovered later.
	n.advertised = []netip.Prefix{netip.MustParsePrefix("192.168.50.0/24")}
	n.syncUDP()
	if len(listeners) != 1 || listeners[netip.MustParseAddrPort("192.168.50.120:1024")] == nil {
		t.Fatalf("listened on %v, want only the HL2", listeners)
	}
	// A stopped node listens on nothing and can't be made to.
	n.closeUDP()
	for _, l := range listeners {
		if !l.closed() {
			t.Fatal("a stopped node is still listening")
		}
	}
	clear(listeners)
	n.syncUDP()
	if len(listeners) != 0 {
		t.Fatalf("a stopped node listened on %v", listeners)
	}
}

func TestUDPFromRefusedPeersNeverReachesTheHL2(t *testing.T) {
	dev, received := udpEcho(t)
	dialed := false
	for _, tc := range []struct {
		name  string
		allow bool
		radio bool
	}{{"refused by the allowlist", false, false}, {"the HL2 address is the radio's", true, true}} {
		n, listeners := hl2Node(t, dev, tc.allow)
		if tc.radio {
			n.localAddrs = func() []netip.Addr { return []netip.Addr{netip.MustParseAddr("192.168.50.120")} }
		}
		n.dialDeviceUDP = func(netip.AddrPort) (net.Conn, error) { dialed = true; return nil, net.ErrClosed }
		n.syncUDP()
		client, flow := net.Pipe()
		listeners[netip.MustParseAddrPort("192.168.50.120:1024")].conns <- flow
		client.SetDeadline(time.Now().Add(3 * time.Second))
		client.Write([]byte("discover"))
		if _, err := client.Read(make([]byte, 8)); err == nil {
			t.Fatalf("%s: the flow was answered", tc.name)
		}
		if got := n.approvedRoutes(); len(got) != 0 {
			t.Fatalf("%s: a refused flow marked routes in use: %v", tc.name, got)
		}
	}
	if dialed || received() != 0 {
		t.Fatal("a refused flow reached the HL2")
	}
}

// One client can't hold open more LAN sockets than maxUDPFlows.
func TestUDPFlowsPerDeviceAreCapped(t *testing.T) {
	dev, _ := udpEcho(t)
	n, listeners := hl2Node(t, dev, true)
	n.syncUDP()
	l := listeners[netip.MustParseAddrPort("192.168.50.120:1024")]
	var clients []net.Conn
	for i := 0; i < maxUDPFlows+1; i++ {
		c, flow := net.Pipe()
		clients = append(clients, c)
		l.conns <- flow
	}
	defer func() {
		for _, c := range clients {
			c.Close()
		}
	}()
	last := clients[maxUDPFlows]
	last.SetDeadline(time.Now().Add(3 * time.Second))
	if _, err := last.Write([]byte("discover")); err == nil {
		if _, err := last.Read(make([]byte, 8)); err == nil {
			t.Fatalf("flow %d past the cap of %d was relayed", maxUDPFlows+1, maxUDPFlows)
		}
	}
}
