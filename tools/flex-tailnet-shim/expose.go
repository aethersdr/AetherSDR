package main

import (
	"bufio"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/netip"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Published ports. An app running on the radio itself (another container,
// or a legacy waveform) can ask the shim to make one of its TCP ports
// reachable on the tailnet: POST /v1/local/expose from the radio, renewed
// within the lease. The shim then listens on that port on its tailnet
// address and splices each caller the tailnet admits to the app. AetherSDR's
// radio agents (radio health, WSPR) serve their JSON this way to a remote
// operator.
//
// Only a port bound where the radio's LAN could reach it can be published:
// the wildcard, or a LAN address (publishableBind). The shim reads binds, not
// firewall rules, so a wildcard listener a firewall keeps from the LAN
// passes too.

const (
	exposeLease = 10 * time.Minute
	maxExposed  = 8
)

// exposeReserved are ports an app may never publish: SSH, the SmartSDR
// ports and the shim's own.
var exposeReserved = map[int]bool{
	22: true, radioVitaInPort: true, radioAPIPort: true, 4993: true, 4994: true, 42607: true,
	ProvisionPort: true, TelemetryPort: true,
}

var (
	errExposePort      = errors.New("port must be 1024-65535 and not one the radio or the shim uses")
	errExposeFull      = fmt.Errorf("at most %d ports may be published", maxExposed)
	errExposeConflict  = errors.New("another app already published this port")
	errExposeName      = errors.New("name must be 1-32 printable characters")
	errExposeNoService = errors.New("nothing listens on this port beyond loopback")
	errExposeListen    = errors.New("the shim could not listen on this port on the tailnet")
	errPortTaken       = errors.New("the port is already in use on the tailnet")
)

// tailnetPorts makes each dynamic tailnet port exclusive between side
// channels and published ports. tsnet doesn't: a side channel listens on
// ":P" and a published port on "<ip>:P", which are different keys, and the
// exact address silently wins, so a published port could capture a transfer
// and skip its per-session grant. A port is reserved before either listens,
// and stays reserved until that listener closes.
type tailnetPorts struct {
	mu   sync.Mutex
	held map[int]bool
}

func (p *tailnetPorts) listen(port int, open func() (net.Listener, error)) (net.Listener, error) {
	p.mu.Lock()
	if p.held[port] {
		p.mu.Unlock()
		return nil, errPortTaken
	}
	if p.held == nil {
		p.held = map[int]bool{}
	}
	p.held[port] = true
	p.mu.Unlock()
	ln, err := open()
	if err != nil {
		p.release(port)
		return nil, err
	}
	return &heldListener{Listener: ln, release: func() { p.release(port) }}, nil
}

func (p *tailnetPorts) release(port int) {
	p.mu.Lock()
	defer p.mu.Unlock()
	delete(p.held, port)
}

type heldListener struct {
	net.Listener
	once    sync.Once
	release func()
}

func (l *heldListener) Close() error {
	err := l.Listener.Close()
	l.once.Do(l.release)
	return err
}

type exposure struct {
	port    int
	target  netip.Addr // where the app listens: loopback for a wildcard listener
	moves   uint64     // counts target changes, to fence dials in flight across one
	name    string
	expires time.Time
	timer   *time.Timer
	ln      net.Listener
	conns   map[net.Conn]struct{} // spliced connections, closed with the exposure
}

// ExposedPort is one published port, for /v1/status.
type ExposedPort struct {
	Port      int    `json:"port"`
	Name      string `json:"name"`
	Target    string `json:"target"`
	ExpiresIn int    `json:"expires_in_s"`
	Listening bool   `json:"listening"`
}

type exposer struct {
	mu sync.Mutex
	m  map[int]*exposure
	// Set while the tailnet node runs (attach); nil otherwise, when
	// registrations are kept but nothing listens.
	listen    func(port int) (net.Listener, error)
	authorize func(net.Addr) (string, bool)
	splices   *liveSplices
	// Seams for tests; nil means the real ones.
	dial      func(target netip.Addr, port int) (net.Conn, error)
	listening func(port int) (netip.Addr, bool)
	lease     time.Duration
}

func (e *exposer) leaseLen() time.Duration {
	if e.lease > 0 {
		return e.lease
	}
	return exposeLease
}

func validExposeName(s string) bool {
	if len(s) == 0 || len(s) > 32 {
		return false
	}
	for _, r := range s {
		if r < 0x20 || r > 0x7e {
			return false
		}
	}
	return true
}

// register publishes (or renews) port for the app called name and returns
// the lease length and whether the port is listening on the tailnet now (it
// isn't while the tailnet node is down).
func (e *exposer) register(port int, name string) (time.Duration, bool, error) {
	if port < 1024 || port > 65535 || exposeReserved[port] {
		return 0, false, errExposePort
	}
	if !validExposeName(name) {
		return 0, false, errExposeName
	}
	target, ok := e.lookup(port)
	e.mu.Lock()
	defer e.mu.Unlock()
	if !ok {
		// The app is telling us its service is gone: what is still published
		// under its name goes too, so nothing that binds the port later is
		// exposed by a lease it never asked for.
		if x := e.m[port]; x != nil && x.name == name {
			e.dropLocked(port, x)
			log.Printf("expose: TCP %d (%s) withdrawn: nothing listens beyond loopback", port, name)
		}
		return 0, false, errExposeNoService
	}
	if e.m == nil {
		e.m = map[int]*exposure{}
	}
	lease := e.leaseLen()
	x, renew := e.m[port]
	if renew {
		if x.name != name {
			return 0, false, errExposeConflict
		}
		if x.target != target {
			// The app moved (it restarted on another address): splices to
			// the old one go.
			x.target = target
			x.moves++
			x.closeConnsLocked()
		}
		x.expires = time.Now().Add(lease)
		x.timer.Reset(lease)
	} else {
		if len(e.m) >= maxExposed {
			return 0, false, errExposeFull
		}
		x = &exposure{port: port, target: target, name: name, expires: time.Now().Add(lease),
			conns: map[net.Conn]struct{}{}}
		x.timer = time.AfterFunc(lease, func() { e.expire(port, x) })
		e.m[port] = x
		log.Printf("expose: %s published TCP %d (%s)", name, port, target)
	}
	if err := e.openLocked(x); err != nil {
		if !renew {
			x.timer.Stop()
			delete(e.m, port)
		}
		return 0, false, fmt.Errorf("%w: %v", errExposeListen, err)
	}
	return lease, x.ln != nil, nil
}

// expire drops x if it is still port's registration and its lease lapsed,
// closing its listener and every connection spliced through it.
func (e *exposer) expire(port int, x *exposure) {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.m[port] != x || time.Now().Before(x.expires) {
		return
	}
	e.dropLocked(port, x)
	log.Printf("expose: TCP %d (%s) lease lapsed", port, x.name)
}

func (e *exposer) dropLocked(port int, x *exposure) {
	delete(e.m, port)
	x.timer.Stop()
	if x.ln != nil {
		x.ln.Close()
		x.ln = nil
	}
	x.closeConnsLocked()
}

// lookup finds the address port's service can be dialed at now.
func (e *exposer) lookup(port int) (netip.Addr, bool) {
	if e.listening != nil {
		return e.listening(port)
	}
	return serviceAddr(port)
}

// served reports whether the eligible service on port is still at target.
// A lease outlives the app that took it, and another process can bind the
// port meanwhile, perhaps on loopback alone, so a registration proves nothing
// about the connection arriving now.
func (e *exposer) served(port int, target netip.Addr) bool {
	a, ok := e.lookup(port)
	return ok && a == target
}

func (x *exposure) closeConnsLocked() {
	for c := range x.conns {
		c.Close()
	}
	x.conns = map[net.Conn]struct{}{}
}

// openLocked starts x's tailnet listener when the node is running and it
// isn't listening yet. Nil when there is nothing to do.
func (e *exposer) openLocked(x *exposure) error {
	if e.listen == nil || x.ln != nil {
		return nil
	}
	ln, err := e.listen(x.port)
	if err != nil {
		log.Printf("expose: cannot listen on tailnet TCP %d: %v", x.port, err)
		return err
	}
	x.ln = ln
	go e.accept(ln, x)
	return nil
}

// attach opens every registration's listener on a node that just came up.
func (e *exposer) attach(listen func(int) (net.Listener, error), authorize func(net.Addr) (string, bool), splices *liveSplices) {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.listen, e.authorize, e.splices = listen, authorize, splices
	for _, x := range e.m {
		e.openLocked(x)
	}
}

// detach closes the listeners of a node going down; registrations stay.
func (e *exposer) detach() {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.listen, e.authorize = nil, nil
	for _, x := range e.m {
		if x.ln != nil {
			x.ln.Close()
			x.ln = nil
		}
		x.closeConnsLocked()
	}
}

func (e *exposer) list() []ExposedPort {
	e.mu.Lock()
	defer e.mu.Unlock()
	out := []ExposedPort{}
	for _, x := range e.m {
		out = append(out, ExposedPort{Port: x.port, Name: x.name, Target: x.target.String(),
			ExpiresIn: int(time.Until(x.expires).Seconds()), Listening: x.ln != nil})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Port < out[j].Port })
	return out
}

func (e *exposer) accept(ln net.Listener, x *exposure) {
	for {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		go e.splice(c, x, ln)
	}
}

// track adds conns to x's connections if nothing retired them since splice
// took its snapshot: x is still the port's registration, on the same
// listener (a detach and reattach replaces it), with the target unmoved.
func (e *exposer) track(x *exposure, ln net.Listener, moves uint64, conns ...net.Conn) bool {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.m[x.port] != x || x.ln != ln || x.moves != moves {
		return false
	}
	for _, c := range conns {
		x.conns[c] = struct{}{}
	}
	return true
}

func (e *exposer) untrack(x *exposure, conns ...net.Conn) {
	e.mu.Lock()
	defer e.mu.Unlock()
	for _, c := range conns {
		delete(x.conns, c)
	}
}

func (e *exposer) splice(c net.Conn, x *exposure, ln net.Listener) {
	defer c.Close()
	e.mu.Lock()
	authorize, dial, splices, target, moves := e.authorize, e.dial, e.splices, x.target, x.moves
	current := x.ln == ln
	e.mu.Unlock()
	if authorize == nil || !current {
		return
	}
	if who, ok := authorize(c.RemoteAddr()); !ok {
		log.Printf("expose %d: refused %s", x.port, who)
		return
	}
	if !e.served(x.port, target) {
		log.Printf("expose %d: refused %s: the app no longer listens at %s", x.port, c.RemoteAddr(), target)
		return
	}
	if dial == nil {
		dial = func(t netip.Addr, p int) (net.Conn, error) {
			return net.DialTimeout("tcp", netip.AddrPortFrom(t, uint16(p)).String(), 5*time.Second)
		}
	}
	r, err := dial(target, x.port)
	if err != nil {
		log.Printf("expose %d: app at %s unreachable: %v", x.port, target, err)
		return
	}
	defer r.Close()
	// Again now the dial is done: it may have reached whatever took the port
	// while it ran. Either way nothing has been read from r yet.
	if !e.served(x.port, target) || !e.track(x, ln, moves, c, r) {
		return
	}
	defer e.untrack(x, c, r)
	if splices != nil {
		defer splices.add(c.RemoteAddr(), netip.Addr{}, c, r)()
	}
	// Check again now that the connection is registered, so an allowlist
	// saved during the dial above can't miss it (see Forwarder.splice).
	if who, ok := authorize(c.RemoteAddr()); !ok {
		log.Printf("expose %d: %s is no longer admitted", x.port, who)
		return
	}
	done := make(chan struct{}, 2)
	go func() { io.Copy(r, c); closeWrite(r); done <- struct{}{} }()
	go func() { io.Copy(c, r); closeWrite(c); done <- struct{}{} }()
	<-done
	<-done
}

// localCaller reports whether a is on the radio itself: loopback or one of
// the radio's own addresses. An 8000-series container shares the radio's
// network namespace, so it calls from these too; a range such as
// 172.30.0.0/16 is not trusted, because on a FLEX-6000 it is just LAN.
func (n *Node) localCaller(a netip.Addr) bool {
	a = a.Unmap()
	return a.IsLoopback() || n.isRadio(a)
}

// serviceAddr finds a TCP listener on port that is reachable beyond
// loopback, from /proc/net/tcp and tcp6. It returns the address to dial it
// at: loopback for a wildcard listener, else the address it is bound to.
func serviceAddr(port int) (netip.Addr, bool) {
	var found []netip.Addr
	for _, f := range []string{"/proc/net/tcp", "/proc/net/tcp6"} {
		found = append(found, listeners(f, port)...)
	}
	return pickTarget(found)
}

// publishableBind reports whether a service bound to exactly a counts as
// reachable from the LAN. On an 8000-series radio 172.30.0.0/16 is the
// internal container network, so a service bound there is one the radio
// keeps to itself. On a FLEX-6000 (a non-container build) that range is just
// LAN, as localCaller notes, and any address but loopback will do.
func publishableBind(a netip.Addr) bool {
	if buildVariant != "" {
		return !a.IsLoopback()
	}
	return lanAddr(a)
}

func pickTarget(addrs []netip.Addr) (netip.Addr, bool) {
	var specific netip.Addr
	for _, a := range addrs {
		switch {
		case a.IsUnspecified() && a.Is4():
			return netip.MustParseAddr("127.0.0.1"), true
		case a.IsUnspecified():
			return netip.IPv6Loopback(), true
		case publishableBind(a) && (!specific.IsValid() || a.Is4()):
			specific = a
		}
	}
	return specific, specific.IsValid()
}

// listeners returns the local addresses of the LISTEN sockets on port in a
// /proc/net/tcp-format file.
func listeners(path string, port int) []netip.Addr {
	f, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer f.Close()
	return parseListeners(f, port)
}

func parseListeners(r io.Reader, port int) []netip.Addr {
	var out []netip.Addr
	sc := bufio.NewScanner(r)
	for sc.Scan() {
		f := strings.Fields(sc.Text())
		if len(f) < 4 || f[3] != "0A" { // 0A is TCP_LISTEN
			continue
		}
		host, p, ok := strings.Cut(f[1], ":")
		if !ok {
			continue
		}
		if v, err := strconv.ParseUint(p, 16, 16); err != nil || int(v) != port {
			continue
		}
		if a, ok := procAddr(host); ok {
			out = append(out, a)
		}
	}
	return out
}

// procAddr decodes /proc/net/tcp's hex address: 4-byte words in host
// (little-endian) order.
func procAddr(h string) (netip.Addr, bool) {
	b, err := hex.DecodeString(h)
	if err != nil || (len(b) != 4 && len(b) != 16) {
		return netip.Addr{}, false
	}
	for i := 0; i < len(b); i += 4 {
		b[i], b[i+1], b[i+2], b[i+3] = b[i+3], b[i+2], b[i+1], b[i]
	}
	a, _ := netip.AddrFromSlice(b)
	return a.Unmap(), true
}
