package main

import (
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/netip"
	"sort"
	"sync"
	"time"
)

// Published ports. An app running on the radio itself (another container,
// or a legacy waveform) can ask the shim to make one of its TCP ports
// reachable on the tailnet: POST /v1/local/expose from the radio, renewed
// within the lease. The shim then listens on that port on its tailnet
// address and splices each caller the tailnet admits to the app, at the
// address the app registered from. AetherSDR's radio agents (radio health,
// WSPR) serve their JSON this way to a remote operator.

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
	errExposePort     = errors.New("port must be 1024-65535 and not one the radio or the shim uses")
	errExposeFull     = fmt.Errorf("at most %d ports may be published", maxExposed)
	errExposeConflict = errors.New("another app already published this port")
	errExposeName     = errors.New("name must be 1-32 printable characters")
)

type exposure struct {
	port    int
	target  netip.Addr
	name    string
	expires time.Time
	timer   *time.Timer
	ln      net.Listener
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
	// Seams for tests; nil means the real ones.
	dial    func(target netip.Addr, port int) (net.Conn, error)
	lease   time.Duration
	splices *liveSplices
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

// register publishes (or renews) port for an app at target and returns the
// lease length.
func (e *exposer) register(target netip.Addr, port int, name string) (time.Duration, error) {
	if port < 1024 || port > 65535 || exposeReserved[port] {
		return 0, errExposePort
	}
	if !validExposeName(name) {
		return 0, errExposeName
	}
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.m == nil {
		e.m = map[int]*exposure{}
	}
	lease := e.leaseLen()
	if x, ok := e.m[port]; ok {
		if x.target != target {
			return 0, errExposeConflict
		}
		x.name = name
		x.expires = time.Now().Add(lease)
		x.timer.Reset(lease)
		return lease, nil
	}
	if len(e.m) >= maxExposed {
		return 0, errExposeFull
	}
	x := &exposure{port: port, target: target, name: name, expires: time.Now().Add(lease)}
	x.timer = time.AfterFunc(lease, func() { e.expire(port, x) })
	e.m[port] = x
	log.Printf("expose: %s published TCP %d for %s", target, port, name)
	e.openLocked(x)
	return lease, nil
}

// expire drops x if it is still port's registration and its lease lapsed.
func (e *exposer) expire(port int, x *exposure) {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.m[port] != x || time.Now().Before(x.expires) {
		return
	}
	delete(e.m, port)
	if x.ln != nil {
		x.ln.Close()
		x.ln = nil
	}
	log.Printf("expose: TCP %d (%s) lease lapsed", port, x.name)
}

// openLocked starts x's tailnet listener when the node is running.
func (e *exposer) openLocked(x *exposure) {
	if e.listen == nil || x.ln != nil {
		return
	}
	ln, err := e.listen(x.port)
	if err != nil {
		log.Printf("expose: cannot listen on tailnet TCP %d: %v", x.port, err)
		return
	}
	x.ln = ln
	go e.accept(ln, x.port, x.target)
}

// attach opens every registration's listener on a node that just came up.
func (e *exposer) attach(listen func(int) (net.Listener, error), authorize func(net.Addr) (string, bool)) {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.listen, e.authorize = listen, authorize
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

func (e *exposer) accept(ln net.Listener, port int, target netip.Addr) {
	for {
		c, err := ln.Accept()
		if err != nil {
			return
		}
		go e.splice(c, port, target)
	}
}

func (e *exposer) splice(c net.Conn, port int, target netip.Addr) {
	defer c.Close()
	e.mu.Lock()
	authorize, dial := e.authorize, e.dial
	e.mu.Unlock()
	if authorize == nil {
		return
	}
	if who, ok := authorize(c.RemoteAddr()); !ok {
		log.Printf("expose %d: refused %s", port, who)
		return
	}
	if dial == nil {
		dial = func(t netip.Addr, p int) (net.Conn, error) {
			return net.DialTimeout("tcp", netip.AddrPortFrom(t, uint16(p)).String(), 5*time.Second)
		}
	}
	r, err := dial(target, port)
	if err != nil {
		log.Printf("expose %d: app at %s unreachable: %v", port, target, err)
		return
	}
	defer r.Close()
	if e.splices != nil {
		defer e.splices.add(c.RemoteAddr(), netip.Addr{}, c, r)()
	}
	done := make(chan struct{}, 2)
	go func() { io.Copy(r, c); closeWrite(r); done <- struct{}{} }()
	go func() { io.Copy(c, r); closeWrite(c); done <- struct{}{} }()
	<-done
	<-done
}

// localCaller reports whether a is on the radio itself: loopback, the
// radio's internal container network, or one of the radio's own addresses.
func (n *Node) localCaller(a netip.Addr) bool {
	a = a.Unmap()
	return a.IsLoopback() || containerNet.Contains(a) || n.isRadio(a)
}
