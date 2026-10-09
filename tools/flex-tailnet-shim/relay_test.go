package main

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"net/netip"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

// fakeRadio is a loopback stand-in for the SmartSDR API: it records control
// lines, sends VITA-49-sized datagrams to whatever `client udpport` names, and
// records datagrams arriving on its TX port.
type fakeRadio struct {
	api   net.Listener
	tx    *net.UDPConn
	vita  *net.UDPConn
	lines chan string
	txIn  chan *net.UDPAddr
	eof   chan struct{}
}

func newFakeRadio(t *testing.T) *fakeRadio {
	t.Helper()
	api, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	tx, _ := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	vita, _ := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	fr := &fakeRadio{api: api, tx: tx, vita: vita, lines: make(chan string, 64),
		txIn: make(chan *net.UDPAddr, 64), eof: make(chan struct{})}
	go func() {
		c, err := api.Accept()
		if err != nil {
			return
		}
		fmt.Fprint(c, "V1.4.0.0\nH12345678\n")
		sc := bufio.NewScanner(c)
		for sc.Scan() {
			l := sc.Text()
			fr.lines <- l
			// file download: open a side-channel port, announce it, serve data.
			var seq int
			if _, err := fmt.Sscanf(l, "C%d|file download", &seq); err == nil && strings.Contains(l, "|file download") {
				side, err := net.Listen("tcp", "127.0.0.1:0")
				if err == nil {
					go func() {
						sc, err := side.Accept()
						if err == nil {
							sc.Write([]byte("FILEDATA"))
							sc.Close()
						}
						side.Close()
					}()
					fmt.Fprintf(c, "R%d|0|%d\n", seq, side.Addr().(*net.TCPAddr).Port)
				}
			}
			if strings.Contains(l, "|client set enforce_network_mtu=1") {
				var iseq string
				if i := strings.Index(l, "|"); i > 1 {
					iseq = l[1:i]
				}
				fmt.Fprintf(c, "R%s|0|\n", iseq)
			}
			if _, err := fmt.Sscanf(l, "C%d|info", &seq); err == nil {
				fmt.Fprintf(c, "R%d|0|5000\n", seq) // a number that is NOT a port announcement
			}
			if i := strings.Index(l, "client udpport "); i >= 0 {
				var port int
				fmt.Sscanf(l[i+len("client udpport "):], "%d", &port)
				go func() {
					dst := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: port}
					for k := 0; k < 20; k++ {
						vita.WriteToUDP(make([]byte, 1200), dst)
						time.Sleep(5 * time.Millisecond)
					}
				}()
			}
		}
		close(fr.eof)
	}()
	go func() {
		buf := make([]byte, 2048)
		for {
			_, src, err := tx.ReadFromUDP(buf)
			if err != nil {
				return
			}
			fr.txIn <- src
		}
	}()
	t.Cleanup(func() { api.Close(); tx.Close(); vita.Close() })
	return fr
}

func loopUDP(t *testing.T) *net.UDPConn {
	t.Helper()
	c, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Close() })
	return c
}

func TestRelayEndToEnd(t *testing.T) {
	fr := newFakeRadio(t)
	clientTX, clientPrime, clientOut := loopUDP(t), loopUDP(t), loopUDP(t)
	var hostPort int
	relay := &Relay{
		RadioAddr: "127.0.0.1",
		DialRadio: func() (net.Conn, error) { return net.Dial("tcp", fr.api.Addr().String()) },
		HostUDP: func() (*net.UDPConn, error) {
			c, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
			if err == nil {
				hostPort = c.LocalAddr().(*net.UDPAddr).Port
			}
			return c, err
		},
		ClientTX: clientTX, ClientPrime: clientPrime, ClientOut: clientOut,
		MaxMTU:         1200,
		RadioTXPort:    fr.tx.LocalAddr().(*net.UDPAddr).Port,
		RadioPrimePort: fr.tx.LocalAddr().(*net.UDPAddr).Port,
	}
	ln, _ := net.Listen("tcp", "127.0.0.1:0")
	defer ln.Close()
	go relay.Serve(ln)

	// The "AetherSDR" side.
	ctl, err := net.Dial("tcp", ln.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	greet := bufio.NewReader(ctl)
	if l, _ := greet.ReadString('\n'); l != "V1.4.0.0\n" {
		t.Fatalf("greeting not forwarded verbatim: %q", l)
	}
	myUDP := loopUDP(t)
	myPort := myUDP.LocalAddr().(*net.UDPAddr).Port
	fmt.Fprintf(ctl, "C1|client program AetherSDR\n")
	fmt.Fprintf(ctl, "C2|client set enforce_network_mtu=1 network_mtu=1450\n")
	fmt.Fprintf(ctl, "C3|client udpport %d\n", myPort)

	want := []string{
		"C1|client program AetherSDR",
		"C2|client set enforce_network_mtu=1 network_mtu=1200",
		"", // udpport, checked below
	}
	for i, w := range want {
		select {
		case l := <-fr.lines:
			if i == 2 {
				if l != fmt.Sprintf("C3|client udpport %d", hostPort) {
					t.Fatalf("udpport not rewritten to host port %d: %q", hostPort, l)
				}
			} else if l != w {
				t.Fatalf("line %d: got %q want %q", i, l, w)
			}
		case <-time.After(2 * time.Second):
			t.Fatalf("radio never received line %d", i)
		}
	}

	// Radio -> client VITA-49 arrives on the client's own port, from ClientOut.
	myUDP.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 2048)
	n, src, err := myUDP.ReadFromUDP(buf)
	if err != nil || n != 1200 {
		t.Fatalf("no VITA-49 relayed to client: n=%d err=%v", n, err)
	}
	if src.Port != clientOut.LocalAddr().(*net.UDPAddr).Port {
		t.Fatalf("VITA-49 came from %v, want the relay's out socket", src)
	}

	// Client -> radio TX leaves from the session's host socket.
	myUDP.WriteToUDP([]byte("vita-tx"), clientTX.LocalAddr().(*net.UDPAddr))
	select {
	case from := <-fr.txIn:
		if from.Port != hostPort {
			t.Fatalf("TX reached radio from port %d, want host port %d", from.Port, hostPort)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("TX datagram never reached the radio")
	}

	// A stranger's datagram is dropped.
	stranger := loopUDP(t)
	stranger.WriteToUDP([]byte("evil"), clientTX.LocalAddr().(*net.UDPAddr))
	select {
	case from := <-fr.txIn:
		t.Fatalf("unregistered sender reached the radio via %v", from)
	case <-time.After(300 * time.Millisecond):
	}

	// Principle VI: when the client goes away, the radio connection closes.
	ctl.Close()
	select {
	case <-fr.eof:
	case <-time.After(2 * time.Second):
		t.Fatal("radio connection stayed open after the client disconnected")
	}
}

func TestRewriteLeavesOtherLinesAlone(t *testing.T) {
	host, _ := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	defer host.Close()
	s := &Session{relay: &Relay{MaxMTU: 1200}, host: host}
	for _, in := range []string{
		"C9|slice tune 0 14.074\n",
		"C10|client set network_mtu=1000\n", // already below the clamp
		"C11|xmit 0\r\n",
		"C12|client udpport notaport\n",
		"binary\x00bytes\n",
	} {
		out, port := s.rewriteLine([]byte(in))
		if string(out) != in || port != 0 {
			t.Errorf("rewrote %q to %q (port %d)", in, out, port)
		}
	}
}

func TestSideChannelForwarding(t *testing.T) {
	fr := newFakeRadio(t)
	var opened []int
	fwd := &Forwarder{
		Listen: func(port int) (net.Listener, error) {
			opened = append(opened, port)
			// Stand-in for the tailnet address: a different loopback IP,
			// so the forwarder's port can equal the radio's.
			return net.Listen("tcp", fmt.Sprintf("127.0.0.2:%d", port))
		},
		DialRadio: func(port int) (net.Conn, error) {
			return net.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", port))
		},
	}
	relay := &Relay{
		RadioAddr: "127.0.0.1",
		DialRadio: func() (net.Conn, error) { return net.Dial("tcp", fr.api.Addr().String()) },
		HostUDP: func() (*net.UDPConn, error) {
			return net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
		},
		ClientTX: loopUDP(t), ClientPrime: loopUDP(t), ClientOut: loopUDP(t),
		OpenForward: fwd.Open,
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
	rd.ReadString('\n') // V
	rd.ReadString('\n') // H

	fmt.Fprint(ctl, "C3|info\n")
	if l, _ := rd.ReadString('\n'); l != "R3|0|5000\n" {
		t.Fatalf("reply not forwarded verbatim: %q", l)
	}
	fmt.Fprint(ctl, "C4|file download db_package\n")
	l, _ := rd.ReadString('\n')
	var seq, port int
	if _, err := fmt.Sscanf(l, "R%d|0|%d", &seq, &port); err != nil || seq != 4 {
		t.Fatalf("file download reply: %q", l)
	}
	if len(opened) != 1 || opened[0] != port {
		t.Fatalf("forwarders opened %v, want only [%d] (an unrelated reply must not open one)", opened, port)
	}
	// Another tailnet peer can't take the transfer: the port is this
	// session's alone.
	other := net.Dialer{LocalAddr: &net.TCPAddr{IP: net.IPv4(127, 0, 0, 3)}}
	if stolen, err := other.Dial("tcp", fmt.Sprintf("127.0.0.2:%d", port)); err == nil {
		stolen.SetReadDeadline(time.Now().Add(2 * time.Second))
		if got, _ := io.ReadAll(stolen); len(got) > 0 {
			t.Fatalf("another peer read the side channel: %q", got)
		}
		stolen.Close()
	}
	side, err := net.Dial("tcp", fmt.Sprintf("127.0.0.2:%d", port))
	if err != nil {
		t.Fatalf("side channel not forwarded: %v", err)
	}
	defer side.Close()
	side.SetReadDeadline(time.Now().Add(3 * time.Second))
	got, _ := io.ReadAll(side)
	if string(got) != "FILEDATA" {
		t.Fatalf("side channel carried %q", got)
	}
	verbs := strings.Join(relay.RecentCommands(), ",")
	if !strings.Contains(verbs, "file download") || strings.Contains(verbs, "db_package") {
		t.Fatalf("verb log %q must name verbs but never arguments", verbs)
	}
}

// A LAN host that finds a session's host socket cannot feed the remote
// client: only the radio's datagrams are relayed, and the rest are counted.
func TestHostSocketRelaysOnlyTheRadio(t *testing.T) {
	stranger, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 2)})
	if err != nil {
		t.Skipf("needs a second loopback address: %v", err)
	}
	defer stranger.Close()
	fr := newFakeRadio(t)
	clientTX, clientPrime, clientOut := loopUDP(t), loopUDP(t), loopUDP(t)
	var hostPort atomic.Int64
	relay := &Relay{
		RadioAddr: "127.0.0.1",
		DialRadio: func() (net.Conn, error) { return net.Dial("tcp", fr.api.Addr().String()) },
		HostUDP: func() (*net.UDPConn, error) {
			c, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
			if err == nil {
				hostPort.Store(int64(c.LocalAddr().(*net.UDPAddr).Port))
			}
			return c, err
		},
		LocalAddrs: func() []netip.Addr { return []netip.Addr{netip.MustParseAddr("127.0.0.1")} },
		ClientTX:   clientTX, ClientPrime: clientPrime, ClientOut: clientOut,
		RadioTXPort:    fr.tx.LocalAddr().(*net.UDPAddr).Port,
		RadioPrimePort: fr.tx.LocalAddr().(*net.UDPAddr).Port,
	}
	ln, _ := net.Listen("tcp", "127.0.0.1:0")
	defer ln.Close()
	go relay.Serve(ln)
	ctl, err := net.Dial("tcp", ln.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer ctl.Close()
	myUDP := loopUDP(t)
	fmt.Fprintf(ctl, "C1|client udpport %d\n", myUDP.LocalAddr().(*net.UDPAddr).Port)

	buf := make([]byte, 2048)
	myUDP.SetReadDeadline(time.Now().Add(2 * time.Second))
	if n, _, err := myUDP.ReadFromUDP(buf); err != nil || n != 1200 {
		t.Fatalf("the radio's own VITA-49 must still be relayed: n=%d err=%v", n, err)
	}
	stranger.WriteToUDP([]byte("forged"), &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: int(hostPort.Load())})
	deadline := time.Now().Add(500 * time.Millisecond)
	for time.Now().Before(deadline) {
		myUDP.SetReadDeadline(deadline)
		n, _, err := myUDP.ReadFromUDP(buf)
		if err != nil {
			break
		}
		if string(buf[:n]) == "forged" {
			t.Fatal("a stranger's datagram reached the client")
		}
	}
	ss := relay.Sessions()
	if len(ss) != 1 || ss[0].FromOthers != 1 || !strings.HasPrefix(ss[0].LastRejected, "127.0.0.2:") {
		t.Fatalf("the dropped datagram must be counted and named: %+v", ss)
	}
}

// AetherSDR re-sends `client udpport` after a collision; the first port must
// stop feeding the session once the second replaces it.
func TestRepeatedUDPPortReplacesTheOldMapping(t *testing.T) {
	r := &Relay{}
	r.init()
	s := &Session{relay: r, host: loopUDP(t)}
	first := netip.MustParseAddrPort("100.64.0.9:4993")
	second := netip.MustParseAddrPort("100.64.0.9:4994")
	s.bindClientUDP(first)
	s.bindClientUDP(second)
	if r.byClient[first] != nil || r.byClient[second] != s || *s.udpClient.Load() != second {
		t.Fatalf("mappings after a repeated udpport: %v", r.byClient)
	}
	// A mapping another session owns is not this session's to remove.
	other := &Session{relay: r, host: loopUDP(t)}
	other.bindClientUDP(first)
	s.bindClientUDP(first)
	other.bindClientUDP(netip.MustParseAddrPort("100.64.0.10:4993"))
	if r.byClient[first] != s {
		t.Fatalf("a session removed another's mapping: %v", r.byClient)
	}
}
