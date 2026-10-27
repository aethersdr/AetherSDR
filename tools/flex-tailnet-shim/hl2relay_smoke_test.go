//go:build smoke

package main

// A live check of the HL2 UDP relay against a real tailnet, which the unit
// tests stand in for. Two ephemeral tsnet nodes join: "shim" shares a fake
// HL2 at 192.168.99.120 (served from loopback, nothing is broadcast) and
// "client" sends it a Protocol 1 discovery request over the subnet route.
//
//	SMOKE_AUTHKEY_FILE=key.txt go test -tags smoke -run TestHL2RelaySmoke -v -timeout 15m .
//
// The key must be reusable and ephemeral. Approve the 192.168.99.120/32
// route on the shim node in the admin console when the test asks.

import (
	"context"
	"net"
	"net/netip"
	"os"
	"strings"
	"testing"
	"time"

	"tailscale.com/ipn"
	"tailscale.com/tsnet"
)

func TestHL2RelaySmoke(t *testing.T) {
	keyFile := os.Getenv("SMOKE_AUTHKEY_FILE")
	if keyFile == "" {
		t.Skip("set SMOKE_AUTHKEY_FILE")
	}
	b, err := os.ReadFile(keyFile)
	if err != nil {
		t.Fatal(err)
	}
	key := strings.TrimSpace(string(b))
	ctx, cancel := context.WithTimeout(context.Background(), 12*time.Minute)
	defer cancel()
	up := func(name string) *tsnet.Server {
		s := &tsnet.Server{Hostname: name, Dir: t.TempDir(), AuthKey: key, Ephemeral: true, Logf: func(string, ...any) {}}
		if _, err := s.Up(ctx); err != nil {
			t.Fatalf("%s: join tailnet: %v", name, err)
		}
		t.Cleanup(func() { s.Close() })
		return s
	}
	shimSrv := up("hl2-relay-smoke-shim")
	clientSrv := up("hl2-relay-smoke-client")

	// The fake HL2: answers a discovery request as an idle HL2 would.
	dev, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer dev.Close()
	go func() {
		buf := make([]byte, 2048)
		for {
			k, from, err := dev.ReadFromUDPAddrPort(buf)
			if err != nil {
				return
			}
			if k >= 3 && buf[0] == 0xEF && buf[1] == 0xFE && buf[2] == 0x02 {
				dev.WriteToUDPAddrPort(hl2Reply(0x02), from)
			}
		}
	}()
	devAP := netip.MustParseAddrPort(dev.LocalAddr().String())

	hl2 := netip.MustParseAddrPort("192.168.99.120:1024")
	route := netip.PrefixFrom(hl2.Addr(), 32)
	lc, err := shimSrv.LocalClient()
	if err != nil {
		t.Fatal(err)
	}
	disc := &Discovery{}
	disc.note(Device{Kind: hl2Kind, Name: hl2Kind, IP: hl2.Addr().String(), Port: hl2Port, Serial: "00:1C:C0:A2:13:DD", LastSeen: time.Now()})
	n := &Node{Discovery: disc}
	n.authorize = n.authorizer(lc) // real WhoIs on the flow's source
	n.localAddrs = func() []netip.Addr { return nil }
	n.listenRoutedUDP = func(ap netip.AddrPort) (net.Listener, error) { return shimSrv.Listen("udp4", ap.String()) }
	n.dialDeviceUDP = func(netip.AddrPort) (net.Conn, error) {
		return net.DialUDP("udp4", nil, net.UDPAddrFromAddrPort(devAP))
	}
	if _, err := lc.EditPrefs(ctx, &ipn.MaskedPrefs{Prefs: ipn.Prefs{AdvertiseRoutes: []netip.Prefix{route}}, AdvertiseRoutesSet: true}); err != nil {
		t.Fatal(err)
	}
	n.advertised = []netip.Prefix{route}
	n.syncUDP()

	clc, err := clientSrv.LocalClient()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := clc.EditPrefs(ctx, &ipn.MaskedPrefs{Prefs: ipn.Prefs{RouteAll: true}, RouteAllSet: true}); err != nil {
		t.Fatal(err)
	}
	t.Logf("approve %s on hl2-relay-smoke-shim in the admin console", route)
	for {
		st, err := clc.Status(ctx)
		if err != nil {
			t.Fatal(err)
		}
		approved := false
		for _, p := range st.Peer {
			if p.PrimaryRoutes != nil && p.PrimaryRoutes.ContainsFunc(func(r netip.Prefix) bool { return r == route }) {
				approved = true
			}
		}
		if approved {
			break
		}
		select {
		case <-ctx.Done():
			t.Fatal("the route was never approved")
		case <-time.After(2 * time.Second):
		}
	}
	t.Logf("route approved")

	c, err := clientSrv.Dial(ctx, "udp4", hl2.String())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	for i := 0; i < 3; i++ {
		start := time.Now()
		if _, err := c.Write(hl2DiscoveryRequest()); err != nil {
			t.Fatal(err)
		}
		c.SetReadDeadline(time.Now().Add(5 * time.Second))
		buf := make([]byte, 2048)
		k, err := c.Read(buf)
		if err != nil {
			t.Fatalf("discovery %d: no reply: %v", i, err)
		}
		d, ok := parseHL2Reply(buf[:k], hl2)
		if !ok || d.Serial != "00:1C:C0:A2:13:DD" {
			t.Fatalf("discovery %d: reply %x is not the fake HL2's", i, buf[:k])
		}
		t.Logf("discovery %d: HL2 %s answered in %v", i, d.Serial, time.Since(start).Round(time.Millisecond))
	}
	if got := n.approvedRoutes(); len(got) != 1 {
		t.Fatalf("routes in use %v, want %s", got, route)
	}
}
