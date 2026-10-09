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

	mu sync.Mutex
	// active maps each open port to the tailnet addresses allowed on it.
	active map[int]map[netip.Addr]bool
}

// Open starts forwarding port for client, the tailnet address of the session
// whose file transfer the radio announced it for; only that address may
// connect. If the port is already open, client is added to it. The listener
// accepts connections for sideChannelWindow after the last connection ends,
// then closes.
func (f *Forwarder) Open(port int, client netip.Addr) error {
	f.mu.Lock()
	if f.active == nil {
		f.active = map[int]map[netip.Addr]bool{}
	}
	if set, ok := f.active[port]; ok {
		set[client] = true
		f.mu.Unlock()
		return nil
	}
	f.active[port] = map[netip.Addr]bool{client: true}
	f.mu.Unlock()

	ln, err := f.Listen(port)
	if err != nil {
		f.mu.Lock()
		delete(f.active, port)
		f.mu.Unlock()
		return err
	}
	go f.serve(port, ln)
	return nil
}

func (f *Forwarder) serve(port int, ln net.Listener) {
	defer func() {
		ln.Close()
		f.mu.Lock()
		delete(f.active, port)
		f.mu.Unlock()
	}()
	var wg sync.WaitGroup
	deadline := time.AfterFunc(sideChannelWindow, func() { ln.Close() })
	defer deadline.Stop()
	for {
		c, err := ln.Accept()
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
		deadline.Stop()
		wg.Add(1)
		go func() {
			defer wg.Done()
			f.splice(port, c)
			deadline.Reset(sideChannelWindow)
		}()
	}
}

// Forget withdraws client from every open side channel, once its last
// session has closed; a port no session can use stays shut until its window
// lapses.
func (f *Forwarder) Forget(client netip.Addr) {
	f.mu.Lock()
	defer f.mu.Unlock()
	for _, set := range f.active {
		delete(set, client)
	}
}

// admits reports whether remote is a session the port was opened for.
func (f *Forwarder) admits(port int, remote net.Addr) bool {
	ap, ok := addrPortOf(remote)
	if !ok {
		return false
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.active[port][ap.Addr()]
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
