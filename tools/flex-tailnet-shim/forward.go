package main

import (
	"fmt"
	"io"
	"log"
	"net"
	"net/netip"
	"sort"
	"sync"
	"time"
)

// Side channels: the radio opens some TCP ports only on request. A client
// sends `file upload …` or `file download …` on the control connection, the
// radio replies `R<seq>|0|<port>`, and the client then connects to that port
// on the radio's address. Over the tailnet that address is the shim, so the
// shim must listen on the announced port and splice it to the radio.

// sideChannelWindow is how long a forwarder waits for its connection.
const sideChannelWindow = 60 * time.Second

// Forwarder opens short-lived listeners for announced side-channel ports.
type Forwarder struct {
	Listen    func(port int) (net.Listener, error) // on the tailnet
	DialRadio func(port int) (net.Conn, error)
	Authorize func(remote net.Addr) (string, bool)
	// Window overrides sideChannelWindow (tests).
	Window time.Duration

	mu     sync.Mutex
	active map[int]*sideChannel
}

func (f *Forwarder) window() time.Duration {
	if f.Window > 0 {
		return f.Window
	}
	return sideChannelWindow
}

// sideChannel is one open port and the sessions whose transfers opened it.
type sideChannel struct {
	ln       net.Listener
	clients  map[uint64]netip.Addr // session ID -> that session's tailnet address
	deadline *time.Timer           // shuts the channel sideChannelWindow after it goes idle
}

// Open starts forwarding port for session, whose file transfer the radio
// announced it for; only that session's tailnet address, client, may
// connect. If the port is already open, the session is added to it and the
// window starts again, so the new transfer gets its full minute. closed
// reports whether the session has ended: a reply still in flight when its
// session closes must not leave the port open to a computer with no session.
// The listener accepts connections for sideChannelWindow after the last
// announcement or connection, then closes.
func (f *Forwarder) Open(port int, session uint64, client netip.Addr, closed func() bool) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.active == nil {
		f.active = map[int]*sideChannel{}
	}
	sc := f.active[port]
	if sc == nil {
		// A channel shutting down has already left f.active with its
		// listener closed (shut), so the port is free to listen on again.
		ln, err := f.Listen(port)
		if err != nil {
			return err
		}
		sc = &sideChannel{ln: ln, clients: map[uint64]netip.Addr{}}
		sc.deadline = time.AfterFunc(f.window(), func() { f.shut(port, sc) })
		f.active[port] = sc
		go f.serve(port, sc)
	} else {
		sc.deadline.Reset(f.window())
	}
	sc.clients[session] = client
	// The session sets its closed flag before calling Forget, and both run
	// under f.mu here or there: either Forget removes this entry, or the
	// flag is already visible.
	if closed != nil && closed() {
		delete(sc.clients, session)
	}
	return nil
}

// shut closes a side channel and removes it, both under f.mu, so a
// concurrent Open sees either the open channel or a free port, never a
// closing one it could add a session to.
func (f *Forwarder) shut(port int, sc *sideChannel) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.active[port] == sc {
		delete(f.active, port)
	}
	sc.deadline.Stop()
	sc.ln.Close()
}

func (f *Forwarder) serve(port int, sc *sideChannel) {
	defer f.shut(port, sc)
	var wg sync.WaitGroup
	for {
		c, err := sc.ln.Accept()
		if err != nil {
			wg.Wait()
			return
		}
		if !f.admits(port, c.RemoteAddr()) {
			log.Printf("side channel %d: refused %s: not the session that asked for it", port, c.RemoteAddr())
			c.Close()
			continue
		}
		if f.Authorize != nil {
			if who, ok := f.Authorize(c.RemoteAddr()); !ok {
				log.Printf("side channel %d: refused %s (%s)", port, c.RemoteAddr(), who)
				c.Close()
				continue
			}
		}
		sc.deadline.Stop()
		wg.Add(1)
		go func() {
			defer wg.Done()
			f.splice(port, c)
			sc.deadline.Reset(f.window())
		}()
	}
}

// Forget withdraws a closed session from every open side channel. A port no
// session can use stays shut until its window lapses.
func (f *Forwarder) Forget(session uint64) {
	f.mu.Lock()
	defer f.mu.Unlock()
	for _, sc := range f.active {
		delete(sc.clients, session)
	}
}

// admits reports whether remote belongs to a session the port was opened for.
func (f *Forwarder) admits(port int, remote net.Addr) bool {
	ap, ok := addrPortOf(remote)
	if !ok {
		return false
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	sc := f.active[port]
	if sc == nil {
		return false
	}
	for _, c := range sc.clients {
		if c == ap.Addr() {
			return true
		}
	}
	return false
}

func (f *Forwarder) splice(port int, c net.Conn) {
	defer c.Close()
	var r net.Conn
	var err error
	// The radio may open its listener a moment after replying.
	for attempt := 0; attempt < 10; attempt++ {
		if r, err = f.DialRadio(port); err == nil {
			break
		}
		time.Sleep(200 * time.Millisecond)
	}
	if err != nil {
		log.Printf("side channel %d: radio dial failed: %v", port, err)
		return
	}
	defer r.Close()
	done := make(chan struct{}, 2)
	var up, down int64
	go func() { up, _ = io.Copy(r, c); closeWrite(r); done <- struct{}{} }()
	go func() { down, _ = io.Copy(c, r); closeWrite(c); done <- struct{}{} }()
	<-done
	<-done
	log.Printf("side channel %d: %s done (%d bytes to radio, %d from radio)", port, c.RemoteAddr(), up, down)
}

func closeWrite(c net.Conn) {
	if cw, ok := c.(interface{ CloseWrite() error }); ok {
		cw.CloseWrite()
	}
}

// verbLog keeps recently seen command verbs (e.g. "client gui", "file
// download"), never their arguments, so status can show what a client asked
// for without exposing operator data.
type verbLog struct {
	mu     sync.Mutex
	counts map[string]int
}

func (v *verbLog) add(verb string) {
	v.mu.Lock()
	defer v.mu.Unlock()
	if v.counts == nil {
		v.counts = map[string]int{}
	}
	if _, ok := v.counts[verb]; !ok && len(v.counts) >= 64 {
		return
	}
	v.counts[verb]++
}

func (v *verbLog) snapshot() []string {
	v.mu.Lock()
	defer v.mu.Unlock()
	out := make([]string, 0, len(v.counts))
	for k, n := range v.counts {
		out = append(out, fmt.Sprintf("%s ×%d", k, n))
	}
	sort.Strings(out)
	return out
}
