package main

import (
	"errors"
	"log"
	"net"
	"net/netip"
	"sync"
	"sync/atomic"
	"time"
)

// Station-device UDP. tsnet drops routed UDP that no listener claims, and a
// Hermes-Lite 2 speaks only UDP (Protocol 1 on port 1024). For each shared
// HL2 the node listens on the HL2's own address inside tsnet, which hands
// it one connection per remote client flow, and relays each flow through a
// socket on the radio's LAN. The HL2 answers that socket, and the reply goes
// back to the client from the HL2's address, as if the tailnet were the LAN.

const (
	// udpFlowIdle ends a flow with no datagram either way for this long.
	// A running HL2 streams continuously; a stopped one only answers polls.
	udpFlowIdle = 30 * time.Second
	// maxUDPFlows bounds the LAN sockets one device's relay can hold open.
	maxUDPFlows = 16
)

// udpRoute is one listened-on device address and its open flows.
type udpRoute struct {
	ln    net.Listener
	flows atomic.Int32
}

// udpTargets is where UDP is relayed: port 1024 of every discovered HL2
// inside an advertised route.
func (n *Node) udpTargets(advertised []netip.Prefix) map[netip.AddrPort]bool {
	want := map[netip.AddrPort]bool{}
	if n.Discovery == nil {
		return want
	}
	for _, d := range n.Discovery.Devices() {
		if d.Kind != hl2Kind {
			continue
		}
		a, err := netip.ParseAddr(d.IP)
		if err != nil {
			continue
		}
		for _, p := range advertised {
			if p.Contains(a) {
				want[netip.AddrPortFrom(a, hl2Port)] = true
				break
			}
		}
	}
	return want
}

// syncUDP opens and closes UDP listeners to match udpTargets. Called with
// routeMu held, after every route refresh.
func (n *Node) syncUDP() {
	n.mu.Lock()
	listen, adv := n.listenRoutedUDP, n.advertised
	n.mu.Unlock()
	want := map[netip.AddrPort]bool{}
	if listen != nil {
		want = n.udpTargets(adv)
	}
	n.mu.Lock()
	var closing []net.Listener
	for ap, r := range n.udpRoutes {
		if !want[ap] {
			closing = append(closing, r.ln)
			delete(n.udpRoutes, ap)
		}
	}
	var opening []netip.AddrPort
	for ap := range want {
		if _, ok := n.udpRoutes[ap]; !ok {
			opening = append(opening, ap)
		}
	}
	n.mu.Unlock()
	for _, ln := range closing {
		ln.Close()
		log.Printf("subnet udp %s: no longer relayed", ln.Addr())
	}
	for _, ap := range opening {
		ln, err := listen(ap)
		if err != nil {
			log.Printf("subnet udp %s: %v", ap, err)
			continue
		}
		r := &udpRoute{ln: ln}
		n.mu.Lock()
		if n.listenRoutedUDP == nil { // the node stopped meanwhile
			n.mu.Unlock()
			ln.Close()
			continue
		}
		if n.udpRoutes == nil {
			n.udpRoutes = map[netip.AddrPort]*udpRoute{}
		}
		n.udpRoutes[ap] = r
		n.mu.Unlock()
		log.Printf("subnet udp %s: relaying", ap)
		go n.serveUDP(r, ap)
	}
}

// closeUDP closes every UDP listener; their flows end with them.
func (n *Node) closeUDP() {
	n.mu.Lock()
	routes := n.udpRoutes
	n.udpRoutes, n.listenRoutedUDP = nil, nil
	n.mu.Unlock()
	for _, r := range routes {
		r.ln.Close()
	}
}

func (n *Node) serveUDP(r *udpRoute, dst netip.AddrPort) {
	for {
		c, err := r.ln.Accept()
		if err != nil {
			// Forget it, so the next refresh can listen again if the
			// address is still wanted (a listener from a stopped node).
			n.mu.Lock()
			if n.udpRoutes[dst] == r {
				delete(n.udpRoutes, dst)
			}
			n.mu.Unlock()
			return
		}
		if r.flows.Add(1) > maxUDPFlows {
			r.flows.Add(-1)
			log.Printf("subnet udp %s -> %s: refused, already %d flows", c.RemoteAddr(), dst, maxUDPFlows)
			c.Close()
			continue
		}
		go func() {
			defer r.flows.Add(-1)
			n.udpFlow(c, dst)
		}()
	}
}

// udpFlow relays one client's datagrams to dst and dst's back, admitting
// the client as subnetTCP does.
func (n *Node) udpFlow(c net.Conn, dst netip.AddrPort) {
	defer c.Close()
	src := c.RemoteAddr()
	n.mu.Lock()
	authorize := n.authorize
	dial := n.dialDeviceUDP
	n.mu.Unlock()
	if authorize == nil {
		log.Printf("subnet udp %s -> %s: refused, the node is not running", src, dst)
		return
	}
	if who, ok := authorize(src); !ok {
		log.Printf("subnet udp %s -> %s: refused %s", src, dst, who)
		return
	}
	if n.isRadio(dst.Addr()) {
		log.Printf("subnet udp %s -> %s: refused, that is the radio itself", src, dst)
		return
	}
	n.markRouteInUse(dst.Addr())
	if dial == nil {
		dial = func(ap netip.AddrPort) (net.Conn, error) {
			return net.DialUDP("udp4", nil, net.UDPAddrFromAddrPort(ap))
		}
	}
	h, err := dial(dst)
	if err != nil {
		log.Printf("subnet udp %s -> %s: %v", src, dst, err)
		return
	}
	defer h.Close()
	defer n.splices.add(src, dst.Addr(), c, h)()
	// Checked again once registered, as subnetTCP does, so an allowlist or
	// sharing change made during the dial can't miss this flow.
	if who, ok := authorize(src); !ok {
		log.Printf("subnet udp %s -> %s: %s is no longer on the allowlist", src, dst, who)
		return
	}
	if !n.shared(dst.Addr()) {
		log.Printf("subnet udp %s -> %s: no longer shared", src, dst)
		return
	}
	log.Printf("subnet udp %s -> %s: relaying", src, dst)
	var last atomic.Int64
	last.Store(time.Now().UnixNano())
	var up, down atomic.Int64
	pump := func(from, to net.Conn, count *atomic.Int64) {
		buf := make([]byte, 2048)
		for {
			from.SetReadDeadline(time.Now().Add(udpFlowIdle))
			k, err := from.Read(buf)
			var ne net.Error
			if errors.As(err, &ne) && ne.Timeout() &&
				time.Since(time.Unix(0, last.Load())) < udpFlowIdle {
				continue // quiet this way, busy the other
			}
			if err != nil {
				return
			}
			last.Store(time.Now().UnixNano())
			if _, err := to.Write(buf[:k]); err != nil {
				return
			}
			count.Add(int64(k))
		}
	}
	var wg sync.WaitGroup
	wg.Add(2)
	go func() { defer wg.Done(); pump(c, h, &up); h.Close(); c.Close() }()
	go func() { defer wg.Done(); pump(h, c, &down); h.Close(); c.Close() }()
	wg.Wait()
	log.Printf("subnet udp %s -> %s: done (%d bytes to device, %d from device)", src, dst, up.Load(), down.Load())
}

// markRouteInUse records that a route carried an admitted flow, which is
// first-hand evidence the tailnet approved it.
func (n *Node) markRouteInUse(a netip.Addr) {
	n.mu.Lock()
	defer n.mu.Unlock()
	for _, p := range n.advertised {
		if p.Contains(a) {
			if n.routesInUse == nil {
				n.routesInUse = map[netip.Prefix]bool{}
			}
			n.routesInUse[p] = true
			return
		}
	}
}
