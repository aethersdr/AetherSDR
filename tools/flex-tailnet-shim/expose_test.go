package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

var appAddr = netip.MustParseAddr("127.0.0.1")

func TestExposeValidates(t *testing.T) {
	var e exposer
	for _, port := range []int{0, 80, 1023, 4992, 4993, 42607, ProvisionPort, TelemetryPort, 65536} {
		if _, err := e.register(appAddr, port, "app"); err != errExposePort {
			t.Errorf("port %d: got %v, want errExposePort", port, err)
		}
	}
	for _, name := range []string{"", strings.Repeat("x", 33), "bad\nname"} {
		if _, err := e.register(appAddr, 48995, name); err != errExposeName {
			t.Errorf("name %q: got %v, want errExposeName", name, err)
		}
	}
	if _, err := e.register(appAddr, 48995, "AetherWSPR"); err != nil {
		t.Fatal(err)
	}
	// Renewing from the same app is fine; another app may not take the port.
	if _, err := e.register(appAddr, 48995, "AetherWSPR"); err != nil {
		t.Errorf("renew: %v", err)
	}
	if _, err := e.register(netip.MustParseAddr("172.30.1.9"), 48995, "other"); err != errExposeConflict {
		t.Errorf("conflict: got %v", err)
	}
	for p := 50000; p < 50000+maxExposed-1; p++ {
		if _, err := e.register(appAddr, p, "app"); err != nil {
			t.Fatalf("port %d: %v", p, err)
		}
	}
	if _, err := e.register(appAddr, 51000, "app"); err != errExposeFull {
		t.Errorf("cap: got %v", err)
	}
}

func TestExposeLeaseLapses(t *testing.T) {
	e := exposer{lease: 50 * time.Millisecond}
	if _, err := e.register(appAddr, 48995, "app"); err != nil {
		t.Fatal(err)
	}
	time.Sleep(200 * time.Millisecond)
	if got := e.list(); len(got) != 0 {
		t.Fatalf("lapsed lease still listed: %+v", got)
	}
}

// listenLoopback stands in for the tailnet: each published port listens on
// a free loopback port, recorded so the test can dial it.
func listenLoopback(t *testing.T, got map[int]string) func(int) (net.Listener, error) {
	return func(port int) (net.Listener, error) {
		ln, err := net.Listen("tcp", "127.0.0.1:0")
		if err == nil {
			got[port] = ln.Addr().String()
		}
		return ln, err
	}
}

func TestExposeSplicesAdmittedCallers(t *testing.T) {
	app, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer app.Close()
	go func() {
		for {
			c, err := app.Accept()
			if err != nil {
				return
			}
			c.Write([]byte("{\"agent\":\"test\"}\n"))
			c.Close()
		}
	}()
	appPort := app.Addr().(*net.TCPAddr).Port
	var admit atomic.Bool
	admit.Store(true)
	tailnet := map[int]string{}
	e := exposer{dial: func(netip.Addr, int) (net.Conn, error) { return net.Dial("tcp", app.Addr().String()) }}
	// Registered before the node is up: nothing listens yet.
	if _, err := e.register(appAddr, appPort, "app"); err != nil {
		t.Fatal(err)
	}
	if e.list()[0].Listening {
		t.Fatal("listening before attach")
	}
	e.attach(listenLoopback(t, tailnet), func(net.Addr) (string, bool) { return "peer", admit.Load() })
	if !e.list()[0].Listening {
		t.Fatal("not listening after attach")
	}
	read := func() string {
		c, err := net.Dial("tcp", tailnet[appPort])
		if err != nil {
			t.Fatal(err)
		}
		defer c.Close()
		c.SetDeadline(time.Now().Add(2 * time.Second))
		line, _ := bufio.NewReader(c).ReadString('\n')
		return line
	}
	if got := read(); got != "{\"agent\":\"test\"}\n" {
		t.Errorf("admitted caller read %q", got)
	}
	admit.Store(false)
	if got := read(); got != "" {
		t.Errorf("refused caller read %q", got)
	}
	e.detach()
	if e.list()[0].Listening {
		t.Error("still listening after detach")
	}
	if c, err := net.DialTimeout("tcp", tailnet[appPort], time.Second); err == nil {
		c.Close()
		t.Error("detached listener still accepts")
	}
}

func TestLocalExposeAPIOnlyForTheRadio(t *testing.T) {
	n := &Node{RadioAddr: "172.30.1.1", localAddrs: func() []netip.Addr {
		return []netip.Addr{netip.MustParseAddr("192.168.50.100")}
	}}
	h := NewAPI(n)
	post := func(from, body string) int {
		r := httptest.NewRequest("POST", "/v1/local/expose", strings.NewReader(body))
		r.RemoteAddr = from
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		return w.Code
	}
	// Loopback, the container network and the radio's own LAN address are
	// all the radio; each app gets its own port.
	for i, from := range []string{"127.0.0.1:5000", "172.30.1.20:5000", "192.168.50.100:5000"} {
		body := fmt.Sprintf(`{"port":%d,"name":"app%d"}`, 48995+i, i)
		if code := post(from, body); code != http.StatusOK {
			t.Errorf("%s: %d, want 200", from, code)
		}
	}
	body := `{"port":49100,"name":"AetherWSPR"}`
	for _, from := range []string{"192.168.50.7:5000", "100.86.176.80:5000", "8.8.8.8:5000"} {
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
	// still closed to apps on the radio.
	r := httptest.NewRequest("GET", "/v1/status", nil)
	r.RemoteAddr = "192.168.50.7:5000"
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	var st Status
	if w.Code != http.StatusOK || json.Unmarshal(w.Body.Bytes(), &st) != nil || len(st.Exposed) != 3 {
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
