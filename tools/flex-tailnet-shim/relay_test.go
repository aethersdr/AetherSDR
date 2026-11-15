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
		HostUDP: func(netip.Addr) (*net.UDPConn, error) {
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
		HostUDP: func(netip.Addr) (*net.UDPConn, error) {
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
// The radio is recognised either as RadioAddr or, when it sends from another
// of the host's own addresses, through LocalAddrs.
func TestHostSocketRelaysOnlyTheRadio(t *testing.T) {
	for _, tc := range []struct{ name, radioAddr string }{
		{"radio at RadioAddr", "127.0.0.1"},
		{"radio at another host address", "192.0.2.1"},
	} {
		t.Run(tc.name, func(t *testing.T) { hostSocketRelaysOnlyTheRadio(t, tc.radioAddr) })
	}
}

func hostSocketRelaysOnlyTheRadio(t *testing.T, radioAddr string) {
	stranger, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 2)})
	if err != nil {
		t.Skipf("needs a second loopback address: %v", err)
	}
	defer stranger.Close()
	fr := newFakeRadio(t)
	clientTX, clientPrime, clientOut := loopUDP(t), loopUDP(t), loopUDP(t)
	var hostPort atomic.Int64
	var boundTo atomic.Pointer[netip.Addr]
	relay := &Relay{
		RadioAddr: radioAddr,
		DialRadio: func() (net.Conn, error) { return net.Dial("tcp", fr.api.Addr().String()) },
		HostUDP: func(local netip.Addr) (*net.UDPConn, error) {
			boundTo.Store(&local)
			c, err := net.ListenUDP("udp4", net.UDPAddrFromAddrPort(netip.AddrPortFrom(local, 0)))
			if err == nil {
				hostPort.Store(int64(c.LocalAddr().(*net.UDPAddr).Port))
			}
			return c, err
		},
		LocalAddrs:     func() []netip.Addr { return []netip.Addr{netip.MustParseAddr("127.0.0.1")} },
		ClientTX:       clientTX,
		ClientPrime:    clientPrime,
		ClientOut:      clientOut,
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
	if b := boundTo.Load(); b == nil || *b != netip.MustParseAddr("127.0.0.1") {
		t.Fatalf("host socket bound to %v, want the radio connection's local address", b)
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

// A failed address listing keeps the radio recognised rather than dropping
// its datagrams until the next refresh.
func TestFailedAddressListingKeepsThePreviousSet(t *testing.T) {
	addrs := []netip.Addr{netip.MustParseAddr("192.168.50.20")}
	r := &Relay{LocalAddrs: func() []netip.Addr { return addrs }}
	r.init()
	if !r.fromRadio(netip.MustParseAddr("192.168.50.20")) {
		t.Fatal("a host address must be recognised")
	}
	addrs = nil
	r.locals.at = time.Now().Add(-2 * time.Second) // due for a refresh
	if r.fromRadio(netip.MustParseAddr("192.168.50.99")) {
		t.Fatal("an unknown source must be refused")
	}
	if !r.fromRadio(netip.MustParseAddr("192.168.50.20")) {
		t.Fatal("a failed listing dropped the radio's address")
	}
}

// Closing a session withdraws its side-channel access and nobody else's,
// including another session from the same computer.
func TestClosingASessionWithdrawsItsSideChannels(t *testing.T) {
	r := &Relay{}
	r.init()
	var forgotten []uint64
	r.ForgetForward = func(id uint64) { forgotten = append(forgotten, id) }
	ip := netip.MustParseAddr("100.64.0.9")
	mk := func(id uint64) *Session {
		a, b := net.Pipe()
		t.Cleanup(func() { a.Close(); b.Close() })
		s := &Session{id: id, relay: r, client: a, radio: b, host: loopUDP(t), clientIP: ip, done: make(chan struct{})}
		r.sessions[id] = s
		return s
	}
	first, second := mk(1), mk(2)
	first.close("test")
	if len(forgotten) != 1 || forgotten[0] != 1 || !first.closed.Load() || second.closed.Load() {
		t.Fatalf("withdrawn %v after closing session 1", forgotten)
	}

	f := &Forwarder{Listen: func(int) (net.Listener, error) { return net.Listen("tcp", "127.0.0.1:0") }}
	open := func(sess uint64, closed bool) {
		if err := f.Open(5000, sess, ip, func() bool { return closed }); err != nil {
			t.Fatal(err)
		}
	}
	from := &net.TCPAddr{IP: net.IPv4(100, 64, 0, 9), Port: 1}
	claimed := func() bool { _, ok := f.claim(5000, from); return ok }
	open(1, false)
	open(2, false)
	f.Forget(1)
	if !claimed() {
		t.Fatal("session 2 from the same computer lost its transfer when session 1 closed")
	}
	// Each announced transfer admits one connection.
	if claimed() {
		t.Fatal("one grant admitted a second connection")
	}
	// A reply that arrives after its session closed (and after Forget ran)
	// must not leave the port open to that computer.
	open(3, true)
	if claimed() {
		t.Fatal("a closed session's late reply opened the port")
	}
	f.mu.Lock()
	sc := f.active[5000]
	f.mu.Unlock()
	f.shut(5000, sc)
}

// A second transfer announced on an open port gets the full window again.
func TestANewTransferRestartsTheWindow(t *testing.T) {
	f := &Forwarder{
		Listen: func(int) (net.Listener, error) { return net.Listen("tcp", "127.0.0.1:0") },
		Window: time.Second,
	}
	ip := netip.MustParseAddr("100.64.0.9")
	if err := f.Open(5000, 1, ip, nil); err != nil {
		t.Fatal(err)
	}
	time.Sleep(700 * time.Millisecond)
	if err := f.Open(5000, 2, ip, nil); err != nil {
		t.Fatal(err)
	}
	time.Sleep(700 * time.Millisecond) // past the first window, inside the second
	f.mu.Lock()
	sc := f.active[5000]
	f.mu.Unlock()
	if sc == nil {
		t.Fatal("the port closed on the first transfer's window")
	}
	time.Sleep(time.Second)
	f.mu.Lock()
	sc = f.active[5000]
	f.mu.Unlock()
	if sc != nil {
		t.Fatal("the port outlived its window")
	}
}

// A side channel that is shutting down is replaced, not joined: a new
// transfer on the same port gets a working listener.
func TestReopeningAShutSideChannelListensAgain(t *testing.T) {
	var opened int
	f := &Forwarder{Listen: func(port int) (net.Listener, error) {
		opened++
		return net.Listen("tcp", "127.0.0.1:0")
	}}
	ip := netip.MustParseAddr("100.64.0.9")
	if err := f.Open(5000, 1, ip, nil); err != nil {
		t.Fatal(err)
	}
	f.mu.Lock()
	old := f.active[5000]
	f.mu.Unlock()
	f.shut(5000, old) // what the window's deadline does
	if err := f.Open(5000, 2, ip, nil); err != nil {
		t.Fatal(err)
	}
	f.mu.Lock()
	cur := f.active[5000]
	f.mu.Unlock()
	if opened != 2 || cur == nil || cur == old {
		t.Fatalf("listeners opened %d; the new transfer joined the closing channel", opened)
	}
	if c, err := net.Dial("tcp", cur.ln.Addr().String()); err != nil {
		t.Fatalf("the replacement listener doesn't accept: %v", err)
	} else {
		c.Close()
	}
	f.shut(5000, cur)
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

// A connection that passes the allowlist, then has the list narrowed while
// its radio dial is still in flight, is ended once it registers.
func TestSessionConnectingDuringSaveIsEnded(t *testing.T) {
	fr := newFakeRadio(t)
	var allowed atomic.Bool
	allowed.Store(true)
	dialing, release := make(chan struct{}), make(chan struct{})
	relay := &Relay{
		RadioAddr: "127.0.0.1",
		DialRadio: func() (net.Conn, error) {
			close(dialing)
			<-release
			return net.Dial("tcp", fr.api.Addr().String())
		},
		HostUDP: func(netip.Addr) (*net.UDPConn, error) {
			return net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
		},
		ClientTX: loopUDP(t), ClientPrime: loopUDP(t), ClientOut: loopUDP(t),
		Authorize: func(net.Addr) (string, bool) { return "guest", allowed.Load() },
	}
	ln, _ := net.Listen("tcp", "127.0.0.1:0")
	defer ln.Close()
	go relay.Serve(ln)
	ctl, err := net.Dial("tcp", ln.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer ctl.Close()
	<-dialing
	allowed.Store(false) // Save Access List, while the dial is in flight
	if n := relay.Revoke(relay.Authorize); n != 0 {
		t.Fatalf("revocation found %d sessions before registration", n)
	}
	close(release)
	select {
	case <-fr.eof:
	case <-time.After(3 * time.Second):
		t.Fatal("the session registered after the save kept its radio connection")
	}
}

// A failed file-transfer reply ends that command's wait, so a later reply
// with the same sequence number can't open a port; the wait list is bounded.
func TestFailedTransferReplyClearsItsWait(t *testing.T) {
	s := &Session{relay: &Relay{}}
	s.noteCommand([]byte("C7|file download db_package\n"))
	if p := s.sideChannelPort([]byte("R7|50000015|\n")); p != 0 {
		t.Fatalf("a failed reply opened port %d", p)
	}
	if p := s.sideChannelPort([]byte("R7|0|42607\n")); p != 0 {
		t.Fatalf("a stale wait opened port %d", p)
	}
	for i := 0; i < 2*maxPendingTransfers; i++ {
		s.noteCommand([]byte(fmt.Sprintf("C%d|file upload x\n", 100+i)))
	}
	if len(s.pending) > maxPendingTransfers {
		t.Fatalf("%d waits pending, want at most %d", len(s.pending), maxPendingTransfers)
	}
}

// A grant whose caller WhoIs then refuses isn't used up: the computer that
// asked can still connect.
func TestRefusedCallerKeepsTheGrant(t *testing.T) {
	radio, _ := net.Listen("tcp", "127.0.0.1:0")
	defer radio.Close()
	go func() {
		for {
			c, err := radio.Accept()
			if err != nil {
				return
			}
			c.Write([]byte("FILEDATA"))
			c.Close()
		}
	}()
	var refuseOnce atomic.Bool
	refuseOnce.Store(true)
	f := &Forwarder{
		Listen:    func(int) (net.Listener, error) { return net.Listen("tcp", "127.0.0.1:0") },
		DialRadio: func(int) (net.Conn, error) { return net.Dial("tcp", radio.Addr().String()) },
		Authorize: func(net.Addr) (string, bool) { return "x", !refuseOnce.Swap(false) },
	}
	if err := f.Open(5000, 1, netip.MustParseAddr("127.0.0.1"), nil); err != nil {
		t.Fatal(err)
	}
	f.mu.Lock()
	addr := f.active[5000].ln.Addr().String()
	f.mu.Unlock()
	first, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	first.SetReadDeadline(time.Now().Add(2 * time.Second))
	if got, _ := io.ReadAll(first); len(got) != 0 {
		t.Fatalf("the refused caller read %q", got)
	}
	first.Close()
	second, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	second.SetReadDeadline(time.Now().Add(3 * time.Second))
	if got, _ := io.ReadAll(second); string(got) != "FILEDATA" {
		t.Fatalf("after a refusal the grant was gone: read %q", got)
	}
	second.Close()
}
