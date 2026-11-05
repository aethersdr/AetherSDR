package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

var loop4 = netip.MustParseAddr("127.0.0.1")

// anyService reports every port as served by a wildcard listener.
func anyService(int) (netip.Addr, bool) { return loop4, true }

func TestExposeValidates(t *testing.T) {
	e := exposer{listening: anyService}
	for _, port := range []int{0, 80, 1023, 4991, 4992, 4993, 4994, 42607, ProvisionPort, TelemetryPort, 65536} {
		if _, _, err := e.register(port, "app"); err != errExposePort {
			t.Errorf("port %d: got %v, want errExposePort", port, err)
		}
	}
	for _, name := range []string{"", strings.Repeat("x", 33), "bad\nname"} {
		if _, _, err := e.register(48995, name); err != errExposeName {
			t.Errorf("name %q: got %v, want errExposeName", name, err)
		}
	}
	if _, _, err := e.register(48995, "AetherWSPR"); err != nil {
		t.Fatal(err)
	}
	// The same app renews; another app may not take the port.
	if _, _, err := e.register(48995, "AetherWSPR"); err != nil {
		t.Errorf("renew: %v", err)
	}
	if _, _, err := e.register(48995, "other"); err != errExposeConflict {
		t.Errorf("conflict: got %v", err)
	}
	for p := 50000; p < 50000+maxExposed-1; p++ {
		if _, _, err := e.register(p, "app"); err != nil {
			t.Fatalf("port %d: %v", p, err)
		}
	}
	if _, _, err := e.register(51000, "app"); err != errExposeFull {
		t.Errorf("cap: got %v", err)
	}
}

func TestExposeNeedsAServiceBeyondLoopback(t *testing.T) {
	e := exposer{listening: func(int) (netip.Addr, bool) { return netip.Addr{}, false }}
	if _, _, err := e.register(48995, "app"); err != errExposeNoService {
		t.Fatalf("got %v, want errExposeNoService", err)
	}
	if len(e.list()) != 0 {
		t.Error("refused registration listed")
	}
}

func TestParseListeners(t *testing.T) {
	// Real /proc/net/tcp and tcp6 lines: 48995 on 0.0.0.0, 48994 on
	// 127.0.0.1 only, 4992 on 192.168.50.100, and 8080 on ::.
	tcp := `  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 00000000:BF63 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 1 1 0000000000000000 100 0 0 10 0
   1: 0100007F:BF62 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 2 1 0000000000000000 100 0 0 10 0
   2: 6432A8C0:1380 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 3 1 0000000000000000 100 0 0 10 0
   3: 6432A8C0:BF63 0B32A8C0:D431 01 00000000:00000000 00:00000000 00000000     0        0 4 1 0000000000000000 20 4 30 10 -1
`
	tcp6 := `  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 00000000000000000000000000000000:1F90 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 5 1 0000000000000000 100 0 0 10 0
`
	cases := []struct {
		port int
		want string
		ok   bool
	}{{48995, "127.0.0.1", true}, {48994, "", false}, {4992, "192.168.50.100", true}, {8080, "::1", true}, {9999, "", false}}
	for _, c := range cases {
		addrs := append(parseListeners(strings.NewReader(tcp), c.port), parseListeners(strings.NewReader(tcp6), c.port)...)
		got, ok := pickTarget(addrs)
		if ok != c.ok || (ok && got.String() != c.want) {
			t.Errorf("port %d: got %v %v, want %q %v", c.port, got, ok, c.want, c.ok)
		}
	}
}

func TestExposeLeaseLapses(t *testing.T) {
	e := exposer{listening: anyService, lease: 50 * time.Millisecond}
	if _, _, err := e.register(48995, "app"); err != nil {
		t.Fatal(err)
	}
	time.Sleep(200 * time.Millisecond)
	if got := e.list(); len(got) != 0 {
		t.Fatalf("lapsed lease still listed: %+v", got)
	}
}

// listenLoopback stands in for the tailnet: each published port listens on
// a free loopback port, recorded so the test can dial it.
func listenLoopback(got map[int]string, fail *atomic.Bool) func(int) (net.Listener, error) {
	return func(port int) (net.Listener, error) {
		if fail != nil && fail.Load() {
			return nil, errors.New("address in use")
		}
		ln, err := net.Listen("tcp", "127.0.0.1:0")
		if err == nil {
			got[port] = ln.Addr().String()
		}
		return ln, err
	}
}

// echoApp is the app being published: it sends one line, then holds the
// connection open until the peer closes it.
func echoApp(t *testing.T) net.Listener {
	app, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go func() {
		for {
			c, err := app.Accept()
			if err != nil {
				return
			}
			go func() {
				defer c.Close()
				c.Write([]byte("{\"agent\":\"test\"}\n"))
				io.Copy(io.Discard, c)
			}()
		}
	}()
	return app
}

func dialApp(app net.Listener) func(netip.Addr, int) (net.Conn, error) {
	return func(netip.Addr, int) (net.Conn, error) { return net.Dial("tcp", app.Addr().String()) }
}

func readFirstLine(t *testing.T, addr string) (string, net.Conn) {
	c, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	c.SetDeadline(time.Now().Add(2 * time.Second))
	line, _ := bufio.NewReader(c).ReadString('\n')
	return line, c
}

func TestExposeSplicesAdmittedCallers(t *testing.T) {
	app := echoApp(t)
	defer app.Close()
	var admit atomic.Bool
	admit.Store(true)
	tailnet := map[int]string{}
	e := exposer{listening: anyService, dial: dialApp(app)}
	// Registered before the node is up: nothing listens yet.
	if _, listening, err := e.register(48995, "app"); err != nil || listening {
		t.Fatalf("before attach: listening=%v err=%v", listening, err)
	}
	e.attach(listenLoopback(tailnet, nil), func(net.Addr) (string, bool) { return "peer", admit.Load() }, &liveSplices{})
	if !e.list()[0].Listening {
		t.Fatal("not listening after attach")
	}
	got, c := readFirstLine(t, tailnet[48995])
	c.Close()
	if got != "{\"agent\":\"test\"}\n" {
		t.Errorf("admitted caller read %q", got)
	}
	admit.Store(false)
	got, c = readFirstLine(t, tailnet[48995])
	c.Close()
	if got != "" {
		t.Errorf("refused caller read %q", got)
	}
	e.detach()
	if e.list()[0].Listening {
		t.Error("still listening after detach")
	}
	if c, err := net.DialTimeout("tcp", tailnet[48995], time.Second); err == nil {
		c.Close()
		t.Error("detached listener still accepts")
	}
}

func TestExposeListenFailureIsReportedAndRetried(t *testing.T) {
	var fail atomic.Bool
	fail.Store(true)
	tailnet := map[int]string{}
	e := exposer{listening: anyService}
	e.attach(listenLoopback(tailnet, &fail), func(net.Addr) (string, bool) { return "peer", true }, nil)
	if _, _, err := e.register(48995, "app"); !errors.Is(err, errExposeListen) {
		t.Fatalf("first registration: got %v, want errExposeListen", err)
	}
	if len(e.list()) != 0 {
		t.Fatal("a registration that never listened is kept")
	}
	fail.Store(false)
	if _, listening, err := e.register(48995, "app"); err != nil || !listening {
		t.Fatalf("once the port is free: listening=%v err=%v", listening, err)
	}
	// A renewal after the listener died reopens it.
	e.mu.Lock()
	e.m[48995].ln.Close()
	e.m[48995].ln = nil
	e.mu.Unlock()
	if _, listening, err := e.register(48995, "app"); err != nil || !listening {
		t.Fatalf("renewal: listening=%v err=%v", listening, err)
	}
}

func TestExposeLapseClosesOpenConnections(t *testing.T) {
	app := echoApp(t)
	defer app.Close()
	tailnet := map[int]string{}
	e := exposer{listening: anyService, dial: dialApp(app), lease: 300 * time.Millisecond}
	e.attach(listenLoopback(tailnet, nil), func(net.Addr) (string, bool) { return "peer", true }, &liveSplices{})
	if _, _, err := e.register(48995, "app"); err != nil {
		t.Fatal(err)
	}
	c, err := net.Dial("tcp", tailnet[48995])
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(3 * time.Second))
	br := bufio.NewReader(c)
	if line, _ := br.ReadString('\n'); line == "" {
		t.Fatal("no data before the lease lapsed")
	}
	_, err = br.ReadByte()
	if ne, ok := err.(net.Error); err == nil || (ok && ne.Timeout()) {
		t.Fatalf("connection still open after the lease lapsed: %v", err)
	}
}

func TestLocalExposeAPIOnlyForTheRadio(t *testing.T) {
	n := &Node{RadioAddr: "172.30.1.1", localAddrs: func() []netip.Addr {
		return []netip.Addr{netip.MustParseAddr("192.168.50.100"), netip.MustParseAddr("172.30.2.1")}
	}}
	n.exposed.listening = anyService
	h := NewAPI(n)
	post := func(from, body string) int {
		r := httptest.NewRequest("POST", "/v1/local/expose", strings.NewReader(body))
		r.RemoteAddr = from
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		return w.Code
	}
	// Loopback, the radio address the radio names and the radio's own
	// interface addresses are all the radio.
	for i, from := range []string{"127.0.0.1:5000", "172.30.1.1:5000", "172.30.2.1:5000", "192.168.50.100:5000"} {
		body := fmt.Sprintf(`{"port":%d,"name":"app%d"}`, 48995+i, i)
		if code := post(from, body); code != http.StatusOK {
			t.Errorf("%s: %d, want 200", from, code)
		}
	}
	// Anything else is refused, including other 172.30.x hosts: on a
	// FLEX-6000 that range is just LAN.
	body := `{"port":49100,"name":"AetherWSPR"}`
	for _, from := range []string{"172.30.1.20:5000", "192.168.50.7:5000", "100.86.176.80:5000", "8.8.8.8:5000"} {
		if code := post(from, body); code != http.StatusForbidden {
			t.Errorf("%s: %d, want 403", from, code)
		}
	}
	if code := post("127.0.0.1:5000", `{"port":4992,"name":"x"}`); code != http.StatusBadRequest {
		t.Errorf("reserved port: %d", code)
	}
	if code := post("127.0.0.1:5000", `{"port":48995,"name":"x","extra":1}`); code != http.StatusBadRequest {
		t.Errorf("unknown field: %d", code)
	}
	// A LAN caller still gets the provisioning API, and the LAN API is
	// still closed to the container network.
	r := httptest.NewRequest("GET", "/v1/status", nil)
	r.RemoteAddr = "192.168.50.7:5000"
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	var st Status
	if w.Code != http.StatusOK || json.Unmarshal(w.Body.Bytes(), &st) != nil || len(st.Exposed) != 4 {
		t.Errorf("LAN status: %d %s", w.Code, w.Body.String())
	}
	r = httptest.NewRequest("GET", "/v1/status", nil)
	r.RemoteAddr = "172.30.1.20:5000"
	w = httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != http.StatusForbidden {
		t.Errorf("container caller on the LAN API: %d, want 403", w.Code)
	}
}

func TestPickTargetCountsOnlyLANBinds(t *testing.T) {
	addr := netip.MustParseAddr
	defer func(v string) { buildVariant = v }(buildVariant)
	for _, tc := range []struct {
		variant string
		bind    string
		want    string // "" refused
	}{
		{"", "0.0.0.0", "127.0.0.1"},
		{"", "::", "::1"},
		{"", "127.0.0.1", ""},
		{"", "192.168.50.100", "192.168.50.100"},
		{"", "169.254.1.1", "169.254.1.1"},
		{"", "172.30.1.1", ""}, // the 8000's container network
		{"", "8.8.8.8", ""},
		{"-native", "172.30.1.1", "172.30.1.1"}, // plain LAN on a FLEX-6000
		{"-native", "127.0.0.1", ""},
		{"-native", "0.0.0.0", "127.0.0.1"},
	} {
		buildVariant = tc.variant
		got, ok := pickTarget([]netip.Addr{addr(tc.bind)})
		if tc.want == "" {
			if ok {
				t.Errorf("%q %s: published at %v", tc.variant, tc.bind, got)
			}
		} else if !ok || got != addr(tc.want) {
			t.Errorf("%q %s: got %v %v, want %s", tc.variant, tc.bind, got, ok, tc.want)
		}
	}
}

func TestServiceAddrReadsRealListeners(t *testing.T) {
	if _, err := os.Stat("/proc/net/tcp"); err != nil {
		t.Skip("no /proc/net/tcp:", err)
	}
	wild, err := net.Listen("tcp4", "0.0.0.0:0")
	if err != nil {
		t.Skip("cannot listen:", err)
	}
	defer wild.Close()
	local, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Skip("cannot listen:", err)
	}
	defer local.Close()
	if a, ok := serviceAddr(wild.Addr().(*net.TCPAddr).Port); !ok || a != loop4 {
		t.Errorf("wildcard listener: got %v %v, want 127.0.0.1", a, ok)
	}
	if a, ok := serviceAddr(local.Addr().(*net.TCPAddr).Port); ok {
		t.Errorf("loopback-only listener was offered for publishing at %v", a)
	}
}

// serve answers every connection on ln with line, until ln closes.
func serve(ln net.Listener, line string) {
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			fmt.Fprintln(c, line)
			c.Close()
		}
	}()
}

// readAll connects to addr and returns all it sends.
func readAll(t *testing.T, addr string) string {
	t.Helper()
	c, err := net.DialTimeout("tcp", addr, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(3 * time.Second))
	b, _ := io.ReadAll(c)
	return string(b)
}

// A service that goes and comes back on loopback alone isn't reachable
// through the lease it left, whether or not anyone renews it.
func TestExposeNeverReachesALoopbackReplacement(t *testing.T) {
	if _, err := os.Stat("/proc/net/tcp"); err != nil {
		t.Skip("no /proc/net/tcp:", err)
	}
	public, err := net.Listen("tcp4", "0.0.0.0:0")
	if err != nil {
		t.Skip("cannot listen:", err)
	}
	serve(public, "public")
	port := public.Addr().(*net.TCPAddr).Port
	got := map[int]string{}
	var fail atomic.Bool
	e := exposer{}
	e.attach(listenLoopback(got, &fail), func(net.Addr) (string, bool) { return "peer", true }, nil)
	defer e.detach()
	if _, _, err := e.register(port, "app"); err != nil {
		t.Fatal(err)
	}
	if s := readAll(t, got[port]); !strings.Contains(s, "public") {
		t.Fatalf("the published service: got %q", s)
	}
	public.Close()
	private, err := net.Listen("tcp4", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		t.Skip("cannot rebind:", err)
	}
	defer private.Close()
	serve(private, "private")
	if s := readAll(t, got[port]); strings.Contains(s, "private") {
		t.Fatal("the lease reached a loopback-only service")
	}
	if _, _, err := e.register(port, "other"); err != errExposeNoService {
		t.Fatalf("another name: got %v", err)
	}
	if len(e.list()) != 1 {
		t.Fatal("another app's failed registration withdrew the publication")
	}
	if _, _, err := e.register(port, "app"); err != errExposeNoService {
		t.Fatalf("renewal: got %v", err)
	}
	if len(e.list()) != 0 {
		t.Fatal("a renewal that found no service left the publication up")
	}
	if c, err := net.DialTimeout("tcp", got[port], time.Second); err == nil {
		c.Close()
		t.Fatal("the withdrawn port still listens")
	}
}

// dialGate holds every dial until released, and records the upstream end.
type dialGate struct {
	entered chan struct{}
	release chan struct{}
	mu      sync.Mutex
	far     net.Conn
}

func newDialGate() *dialGate {
	return &dialGate{entered: make(chan struct{}, 1), release: make(chan struct{})}
}

func (g *dialGate) dial(netip.Addr, int) (net.Conn, error) {
	near, far := net.Pipe()
	g.mu.Lock()
	g.far = far
	g.mu.Unlock()
	g.entered <- struct{}{}
	<-g.release
	return near, nil
}

// upstreamClosed reports that the held dial's connection was abandoned.
func (g *dialGate) upstreamClosed(t *testing.T) bool {
	t.Helper()
	g.mu.Lock()
	far := g.far
	g.mu.Unlock()
	far.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, err := far.Read(make([]byte, 1))
	return err == io.EOF
}

// A dial in flight across something that retires the publication's
// connections doesn't start forwarding afterward.
func TestExposeFencesDialsInFlight(t *testing.T) {
	a, b := netip.MustParseAddr("192.0.2.1"), netip.MustParseAddr("192.0.2.2")
	admit := func(net.Addr) (string, bool) { return "peer", true }
	cases := []struct {
		name string
		// after the dial is held, with `at` the current service address.
		event func(e *exposer, at *atomic.Pointer[netip.Addr], listen func(int) (net.Listener, error))
	}{
		{"target moved", func(e *exposer, at *atomic.Pointer[netip.Addr], _ func(int) (net.Listener, error)) {
			at.Store(&b)
			e.register(48995, "app")
		}},
		// Back on the first address, so only the generation can tell.
		{"target moved and back", func(e *exposer, at *atomic.Pointer[netip.Addr], _ func(int) (net.Listener, error)) {
			at.Store(&b)
			e.register(48995, "app")
			at.Store(&a)
			e.register(48995, "app")
		}},
		{"node restarted", func(e *exposer, _ *atomic.Pointer[netip.Addr], listen func(int) (net.Listener, error)) {
			e.detach()
			e.attach(listen, admit, nil)
		}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var at atomic.Pointer[netip.Addr]
			at.Store(&a)
			gate := newDialGate()
			got := map[int]string{}
			var fail atomic.Bool
			listen := listenLoopback(got, &fail)
			e := exposer{dial: gate.dial, listening: func(int) (netip.Addr, bool) { return *at.Load(), true }}
			e.attach(listen, admit, nil)
			defer e.detach()
			if _, _, err := e.register(48995, "app"); err != nil {
				t.Fatal(err)
			}
			c, err := net.Dial("tcp", got[48995])
			if err != nil {
				t.Fatal(err)
			}
			defer c.Close()
			<-gate.entered
			tc.event(&e, &at, listen)
			close(gate.release)
			if !gate.upstreamClosed(t) {
				t.Fatal("the held dial went on to forward")
			}
		})
	}
}

// The tsnet behavior this guards: both listeners always succeed, so without
// the reservation neither side would ever see the other. openAnyway stands
// in for it.
func TestPublishedPortsAndSideChannelsExcludeEachOther(t *testing.T) {
	openAnyway := func() (net.Listener, error) { return net.Listen("tcp4", "127.0.0.1:0") }
	var ports tailnetPorts
	fwd := &Forwarder{Listen: func(p int) (net.Listener, error) { return ports.listen(p, openAnyway) }}
	e := exposer{listening: anyService}
	e.attach(func(p int) (net.Listener, error) { return ports.listen(p, openAnyway) },
		func(net.Addr) (string, bool) { return "peer", true }, nil)
	defer e.detach()

	// A transfer holds the port: publishing it fails, and works afterward.
	if err := fwd.Open(48995, 1, loop4, nil); err != nil {
		t.Fatal(err)
	}
	if _, _, err := e.register(48995, "app"); !errors.Is(err, errExposeListen) {
		t.Fatalf("publishing a transfer's port: got %v", err)
	}
	if len(e.list()) != 0 {
		t.Fatal("the refused publication was kept")
	}
	fwd.mu.Lock()
	sc := fwd.active[48995]
	fwd.mu.Unlock()
	fwd.shut(48995, sc)
	if _, listening, err := e.register(48995, "app"); err != nil || !listening {
		t.Fatalf("publishing once the transfer ended: %v %v", listening, err)
	}

	// A published port is held: the transfer's listener is refused, and the
	// same goes the other way once the publication is gone.
	if err := fwd.Open(48995, 2, loop4, nil); !errors.Is(err, errPortTaken) {
		t.Fatalf("a side channel on a published port: got %v", err)
	}
	e.detach()
	if err := fwd.Open(48995, 2, loop4, nil); err != nil {
		t.Fatalf("a side channel after the publication closed: %v", err)
	}
}
