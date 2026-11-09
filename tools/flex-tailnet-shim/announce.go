package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"io"
	"log"
	"net"
	"net/netip"
	"regexp"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"

	"tailscale.com/ipn/ipnstate"
)

// Radio announcement over the tailnet. A FlexRadio announces itself about
// once a second by UDP broadcast to port 4992 on its LAN, and a client that
// can't connect by address finds radios only that way: SmartSDR for Windows
// can't. A tailnet carries no broadcast, so the shim forwards each of the
// radio's announcements to the tailnet's devices, one copy addressed to
// each, with ip= naming the shim. Clients take any datagram that arrives on
// 4992 (they listen on the port, they don't ask), so a copy addressed to
// them reads as the broadcast.
//
// A copy addressed to a machine reaches only one of the programs listening
// on 4992 there, and SmartSDR for Windows runs three (SmartSDR, DAX, CAT).
// So a Windows PC runs a discovery helper (cmd/discovery-helper) instead:
// it connects to the shim over the tailnet, receives each announcement, and
// broadcasts it on the PC, where every listener gets it. A device with a
// helper connected is not also sent a copy. The helper opens the
// connection, so it needs no tailnet access rule beyond reaching the radio.
//
// The sockets that hear the announcement are bound to broadcast addresses
// only. The radio's own UDP 4992 receives the clients' registration
// datagrams; a socket bound to the radio's address or the wildcard could
// take those from it, one bound to a broadcast address never matches them.

const (
	announcePort = 4992
	// announceRefresh is how often the device list is re-read from the
	// tailnet backend; announcements in between go to the last list.
	announceRefresh = 10 * time.Second
	// announceInterval is the least time between forwarded announcements.
	// The radio announces about once a second; this bounds what anything
	// else broadcasting to 4992 as the radio could make the shim send.
	announceInterval = 500 * time.Millisecond
	// maxAnnounceTargets bounds the copies sent per announcement. The
	// devices past it are listed in status as over the limit.
	maxAnnounceTargets = 32
	// AnnounceFeedPort is the tailnet TCP port discovery helpers connect
	// to. Each announcement is sent as a 2-byte big-endian length and the
	// packet.
	AnnounceFeedPort = 48994
	maxHelpers       = 16
	// helperFresh: a helper that connects is sent the last announcement at
	// once if it is this recent, so the radio is listed without waiting.
	helperFresh = 3 * time.Second
)

// Announcement modes (provisionConfig.Announce; "" is stored for all).
const (
	announceAll  = "all"  // every online tailnet device not at the station
	announceOnly = "only" // only the devices listed in AnnounceTo
	announceOff  = "off"
)

var (
	announceIPRe     = regexp.MustCompile(`(^| )ip=[^ \x00]*`)
	announceSerialRe = regexp.MustCompile(`(^| )serial=[^ \x00]`)
)

// flexOUI is FlexRadio's IEEE OUI, in a VITA-49 class ID.
const flexOUI = 0x001C2D

// rewriteAnnouncement returns a copy of a radio announcement with ip= set to
// addr, and the address the radio named. A VITA-49 announcement (as radios
// send it) keeps its header and trailer, with the packet size corrected; a
// bare key=value one (as simulators send it) is rewritten as text. Anything
// without a serial= and an ip= is refused.
func rewriteAnnouncement(b []byte, addr netip.Addr) (out []byte, named netip.Addr, ok bool) {
	head, payload, tail := splitVITA(b)
	payload = bytes.TrimRight(payload, "\x00")
	m := announceIPRe.FindSubmatchIndex(payload)
	if m == nil || !announceSerialRe.Match(payload) {
		return nil, netip.Addr{}, false
	}
	named, _ = netip.ParseAddr(string(payload[m[3]+len("ip=") : m[1]]))
	text := announceIPRe.ReplaceAll(payload, []byte("${1}ip="+addr.String()))
	if head == nil {
		return text, named, true
	}
	out = make([]byte, 0, len(head)+len(text)+3+len(tail))
	out = append(out, head...)
	out = append(out, text...)
	for len(out)%4 != 0 {
		out = append(out, 0)
	}
	out = append(out, tail...)
	binary.BigEndian.PutUint16(out[2:4], uint16(len(out)/4))
	return out, named, true
}

// splitVITA separates a FlexRadio VITA-49 packet into header, payload and
// trailer. A packet that isn't one (no class ID, another vendor, a size
// that doesn't fit) comes back as all payload, with a nil header.
func splitVITA(b []byte) (head, payload, tail []byte) {
	if len(b) < 4 {
		return nil, b, nil
	}
	typ, hasClass, hasTrailer := b[0]>>4, b[0]&0x08 != 0, b[0]&0x04 != 0
	tsi, tsf := (b[1]>>6)&3, (b[1]>>4)&3
	size := int(binary.BigEndian.Uint16(b[2:4])) * 4
	n := 4
	if typ == 1 || typ == 3 { // with stream ID
		n += 4
	}
	classAt := n
	if hasClass {
		n += 8
	}
	if tsi != 0 {
		n += 4
	}
	if tsf != 0 {
		n += 8
	}
	end := size
	if hasTrailer {
		end -= 4
	}
	if !hasClass || typ > 5 || size > len(b) || end < n ||
		binary.BigEndian.Uint32(b[classAt:])&0x00FFFFFF != flexOUI {
		return nil, b, nil
	}
	return b[:n], b[n:end], b[end:size]
}

// tailPeer is one tailnet device, as the announcer needs it.
type tailPeer struct {
	ID     string // the tailnet's stable node ID
	Name   string
	OS     string
	Online bool
	IP     netip.Addr // its tailnet IPv4
	Direct netip.Addr // the address its direct path reaches; invalid when relayed or idle
}

func peersFromStatus(st *ipnstate.Status) []tailPeer {
	var out []tailPeer
	for _, p := range st.Peer {
		tp := tailPeer{ID: string(p.ID), OS: p.OS, Online: p.Online}
		tp.Name, _, _ = strings.Cut(p.DNSName, ".")
		if tp.Name == "" {
			tp.Name = p.HostName
		}
		for _, a := range p.TailscaleIPs {
			if a.Is4() {
				tp.IP = a
				break
			}
		}
		if ap, err := netip.ParseAddrPort(p.CurAddr); err == nil {
			tp.Direct = ap.Addr().Unmap()
		}
		out = append(out, tp)
	}
	return out
}

// AnnouncePeer is one tailnet device in /v1/status, with whether it is sent
// the radio's announcement and, if not, why.
type AnnouncePeer struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	OS        string `json:"os"`
	Online    bool   `json:"online"`
	Announced bool   `json:"announced"`
	// Via: "helper" when the device's discovery helper receives it.
	Via string `json:"via,omitempty"`
	// Skipped: "offline", "at the station" (it hears the radio's own
	// broadcast), "not chosen", "off", "over the limit", "no IPv4".
	Skipped string `json:"skipped,omitempty"`
}

// selectTargets decides which devices are sent the announcement. A device
// whose direct path reaches it at a station address is at the station and
// already hears the radio; a second announcement naming the tailnet would
// only make its radio list flip between the two addresses.
// A device whose discovery helper is connected gets the announcement from
// the helper, whatever the device choice says.
func selectTargets(peers []tailPeer, mode string, only []string, station []netip.Prefix, helpers map[netip.Addr]bool) ([]netip.Addr, []AnnouncePeer) {
	sort.Slice(peers, func(i, j int) bool {
		if peers[i].Name != peers[j].Name {
			return peers[i].Name < peers[j].Name
		}
		return peers[i].ID < peers[j].ID
	})
	chosen := map[string]bool{}
	for _, id := range only {
		chosen[id] = true
	}
	var targets []netip.Addr
	report := make([]AnnouncePeer, 0, len(peers))
	for _, p := range peers {
		r := AnnouncePeer{ID: p.ID, Name: p.Name, OS: p.OS, Online: p.Online}
		switch {
		case mode == announceOff:
			r.Skipped = "off"
		case p.IP.IsValid() && helpers[p.IP]:
			r.Announced, r.Via = true, "helper"
		case mode == announceOnly && !chosen[p.ID]:
			r.Skipped = "not chosen"
		case !p.Online:
			r.Skipped = "offline"
		case !p.IP.IsValid():
			r.Skipped = "no IPv4"
		case p.Direct.IsValid() && inAny(station, p.Direct):
			r.Skipped = "at the station"
		case len(targets) >= maxAnnounceTargets:
			r.Skipped = "over the limit"
		default:
			r.Announced = true
			targets = append(targets, p.IP)
		}
		report = append(report, r)
	}
	return targets, report
}

func inAny(ps []netip.Prefix, a netip.Addr) bool {
	for _, p := range ps {
		if p.Contains(a) {
			return true
		}
	}
	return false
}

// Announcer hears the radio's announcements on the LAN and, while the node
// is on the tailnet, forwards them to the tailnet's devices.
type Announcer struct {
	// IsRadio reports whether a LAN source is this radio. Other radios on
	// the station LAN announce too; forwarding theirs with ip= naming this
	// shim would send their users to the wrong radio.
	IsRadio func(netip.Addr) bool
	// Listen opens the sockets the announcement arrives on;
	// listenAnnouncements when nil. Interfaces lists the host's own
	// networks; interfacePrefixes when nil. Test seams.
	Listen     func() []net.PacketConn
	Interfaces func() []netip.Prefix

	mu   sync.Mutex
	link *announceLink // while the node is on the tailnet
	// The radio's last announcement: when it was heard, and the LAN address
	// it named (which places the station's network).
	heard   time.Time
	radioIP netip.Addr
	sentAt  time.Time
	// The devices to send to, and the radio address they were selected
	// for (the station network depends on it).
	targets     []netip.Addr
	selectedFor netip.Addr
	report      []AnnouncePeer
	// Connected discovery helpers, and the last announcement forwarded
	// (rewritten), for a helper that connects.
	helpers  map[*helperConn]bool
	lastOut  []byte
	lastOutT time.Time
}

// helperConn is one connected discovery helper.
type helperConn struct {
	addr netip.Addr
	conn net.Conn
	ch   chan []byte // announcements to write; full means it is behind
}

// announceLink is what forwarding needs from a running tailnet node.
type announceLink struct {
	self    netip.Addr // the shim's tailnet IPv4, named in ip=
	send    func(b []byte, to netip.AddrPort) error
	peers   func(ctx context.Context) ([]tailPeer, error)
	mode    func() (mode string, only []string)
	cancel  context.CancelFunc
	refresh chan struct{}
	failed  bool // the last send failed; logged once until one succeeds
}

// Start listens for the radio's announcements. A broadcast address that
// can't be bound is logged and skipped.
func (a *Announcer) Start() {
	listen := a.Listen
	if listen == nil {
		listen = listenAnnouncements
	}
	for _, pc := range listen() {
		go a.listen(pc)
	}
}

// listenAnnouncements binds UDP 4992 on the limited broadcast address and
// on each LAN interface's own broadcast address, since a radio could use
// either. SO_REUSEADDR lets the bind share the port with the radio's socket
// where the radio allows that; nothing else sets it, so as not to join the
// radio's socket in a SO_REUSEPORT group that would share its traffic.
func listenAnnouncements() []net.PacketConn {
	addrs := []netip.Addr{netip.AddrFrom4([4]byte{255, 255, 255, 255})}
	ifaces, err := net.Interfaces()
	if err != nil {
		log.Printf("announce: listing interfaces: %v", err)
	}
	for _, ifc := range ifaces {
		if ifc.Flags&net.FlagUp == 0 || ifc.Flags&net.FlagBroadcast == 0 {
			continue
		}
		as, _ := ifc.Addrs()
		for _, ia := range as {
			if p, ok := ia.(*net.IPNet); ok {
				if b, ok := directedBroadcast(p); ok {
					addrs = append(addrs, b)
				}
			}
		}
	}
	lc := net.ListenConfig{Control: reuseAddrControl}
	var out []net.PacketConn
	for _, b := range addrs {
		pc, err := lc.ListenPacket(context.Background(), "udp4", netip.AddrPortFrom(b, announcePort).String())
		if err != nil {
			log.Printf("announce: cannot hear the radio's announcements on %s:%d: %v", b, announcePort, err)
			continue
		}
		out = append(out, pc)
	}
	return out
}

// directedBroadcast is a private IPv4 network's broadcast address; the
// radio's internal container network is not a station LAN.
func directedBroadcast(n *net.IPNet) (netip.Addr, bool) {
	ip, ok := netip.AddrFromSlice(n.IP.To4())
	if !ok || !ip.IsPrivate() || containerNet.Contains(ip) || len(n.Mask) != net.IPv4len {
		return netip.Addr{}, false
	}
	b := ip.As4()
	for i := range b {
		b[i] |= ^n.Mask[i]
	}
	return netip.AddrFrom4(b), true
}

func reuseAddrControl(network, address string, c syscall.RawConn) error {
	var serr error
	if err := c.Control(func(fd uintptr) {
		serr = syscall.SetsockoptInt(int(fd), syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
	}); err != nil {
		return err
	}
	return serr
}

// interfacePrefixes lists the host's own IPv4 networks.
func interfacePrefixes() []netip.Prefix {
	as, err := net.InterfaceAddrs()
	if err != nil {
		return nil
	}
	var out []netip.Prefix
	for _, a := range as {
		if p, ok := a.(*net.IPNet); ok {
			ip, ok := netip.AddrFromSlice(p.IP.To4())
			ones, bits := p.Mask.Size()
			if ok && bits == 32 {
				out = append(out, netip.PrefixFrom(ip, ones).Masked())
			}
		}
	}
	return out
}

func (a *Announcer) listen(pc net.PacketConn) {
	buf := make([]byte, 2048)
	for {
		n, src, err := pc.ReadFrom(buf)
		if err != nil {
			log.Printf("announce: %s ended: %v", pc.LocalAddr(), err)
			return
		}
		ap, ok := addrPortOf(src)
		if !ok || a.IsRadio == nil || !a.IsRadio(ap.Addr()) {
			continue
		}
		a.forward(buf[:n])
	}
}

// forward sends one announcement to the selected devices. An announcement
// naming a radio address the selection wasn't made for (the first one
// heard, or a new DHCP lease) isn't sent: the selection is redone first, so
// a device at the station is never sent one.
func (a *Announcer) forward(b []byte) {
	now := time.Now()
	a.mu.Lock()
	link := a.link
	self := netip.IPv4Unspecified()
	if link != nil {
		self = link.self
	}
	out, named, ok := rewriteAnnouncement(b, self)
	if !ok {
		a.mu.Unlock()
		return
	}
	first := a.heard.IsZero()
	a.heard, a.radioIP = now, named
	stale := link != nil && a.selectedFor != named
	send := link != nil && !stale && now.Sub(a.sentAt) >= announceInterval
	var helpers []*helperConn
	if send {
		a.sentAt, a.lastOut, a.lastOutT = now, out, now
		for h := range a.helpers {
			helpers = append(helpers, h)
		}
	}
	targets := a.targets
	a.mu.Unlock()
	if first {
		log.Printf("announce: hearing the radio's announcements")
	}
	if stale {
		a.Refresh()
	}
	if !send {
		return
	}
	if mode, _ := link.mode(); mode != announceOff {
		for _, h := range helpers {
			select {
			case h.ch <- out:
			default: // behind: it gets the next one
			}
		}
	}
	var failed error
	for _, t := range targets {
		if err := link.send(out, netip.AddrPortFrom(t, announcePort)); err != nil {
			failed = err
		}
	}
	a.mu.Lock()
	if failed != nil && !link.failed {
		log.Printf("announce: sending: %v", failed)
	}
	link.failed = failed != nil
	a.mu.Unlock()
}

// Attach starts forwarding over a running tailnet node; Detach stops it.
func (a *Announcer) Attach(self netip.Addr, send func([]byte, netip.AddrPort) error,
	peers func(context.Context) ([]tailPeer, error), mode func() (string, []string)) {
	ctx, cancel := context.WithCancel(context.Background())
	link := &announceLink{self: self, send: send, peers: peers, mode: mode,
		cancel: cancel, refresh: make(chan struct{}, 1)}
	a.mu.Lock()
	if a.link != nil {
		a.link.cancel()
	}
	a.link, a.targets, a.report, a.selectedFor = link, nil, nil, netip.Addr{}
	a.mu.Unlock()
	go a.keepSelecting(ctx, link)
}

func (a *Announcer) Detach() error {
	a.mu.Lock()
	if a.link != nil {
		a.link.cancel()
	}
	a.link, a.targets, a.report = nil, nil, nil
	for h := range a.helpers {
		h.conn.Close()
	}
	a.mu.Unlock()
	return nil
}

// ServeHelpers accepts discovery helpers on ln until it is closed. Each is
// admitted by authorize (the tailnet identity check every flow gets).
func (a *Announcer) ServeHelpers(ln net.Listener, authorize func(net.Addr) (string, bool)) error {
	for {
		c, err := ln.Accept()
		if err != nil {
			return err
		}
		go a.serveHelper(c, authorize)
	}
}

func (a *Announcer) serveHelper(c net.Conn, authorize func(net.Addr) (string, bool)) {
	defer c.Close()
	ap, ok := addrPortOf(c.RemoteAddr())
	if !ok {
		return
	}
	who, ok := authorize(c.RemoteAddr())
	if !ok {
		log.Printf("announce: refused a discovery helper from %s", who)
		return
	}
	h := &helperConn{addr: ap.Addr().Unmap(), conn: c, ch: make(chan []byte, 2)}
	a.mu.Lock()
	if a.link == nil {
		a.mu.Unlock()
		return // the node is stopping
	}
	if len(a.helpers) >= maxHelpers {
		a.mu.Unlock()
		log.Printf("announce: refused a discovery helper from %s: %d connected", who, maxHelpers)
		return
	}
	if a.helpers == nil {
		a.helpers = map[*helperConn]bool{}
	}
	a.helpers[h] = true
	if a.lastOut != nil && time.Since(a.lastOutT) < helperFresh {
		h.ch <- a.lastOut
	}
	a.mu.Unlock()
	log.Printf("announce: discovery helper connected from %s", who)
	a.Refresh()
	defer func() {
		a.mu.Lock()
		delete(a.helpers, h)
		a.mu.Unlock()
		a.Refresh()
		log.Printf("announce: discovery helper from %s left", who)
	}()

	// The helper never sends; a read returns when it goes away.
	gone := make(chan struct{})
	go func() {
		io.Copy(io.Discard, c)
		close(gone)
	}()
	for {
		select {
		case <-gone:
			return
		case b := <-h.ch:
			frame := make([]byte, 2+len(b))
			binary.BigEndian.PutUint16(frame, uint16(len(b)))
			copy(frame[2:], b)
			c.SetWriteDeadline(time.Now().Add(5 * time.Second))
			if _, err := c.Write(frame); err != nil {
				return
			}
		}
	}
}

// Refresh re-selects the devices now, after a setting changed.
func (a *Announcer) Refresh() {
	a.mu.Lock()
	link := a.link
	a.mu.Unlock()
	if link != nil {
		select {
		case link.refresh <- struct{}{}:
		default:
		}
	}
}

func (a *Announcer) keepSelecting(ctx context.Context, link *announceLink) {
	t := time.NewTicker(announceRefresh)
	defer t.Stop()
	var last []netip.Addr
	unheard := time.AfterFunc(30*time.Second, func() {
		a.mu.Lock()
		never := a.heard.IsZero() && a.link == link
		a.mu.Unlock()
		if never {
			log.Printf("announce: the radio's announcement hasn't been heard; nothing is forwarded")
		}
	})
	defer unheard.Stop()
	for {
		pctx, cancel := context.WithTimeout(ctx, 5*time.Second)
		peers, err := link.peers(pctx)
		cancel()
		if err == nil {
			mode, only := link.mode()
			a.mu.Lock()
			radioIP := a.radioIP
			helpers := map[netip.Addr]bool{}
			for h := range a.helpers {
				helpers[h.addr] = true
			}
			a.mu.Unlock()
			station := a.stationNets(radioIP)
			targets, report := selectTargets(peers, mode, only, station, helpers)
			a.mu.Lock()
			if a.link == link {
				a.targets, a.report, a.selectedFor = targets, report, radioIP
			}
			a.mu.Unlock()
			if !slicesEqualAddr(targets, last) {
				log.Printf("announce: announcing the radio to %d tailnet device(s) (%s)", len(targets), mode)
				last = targets
			}
		}
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		case <-link.refresh:
		}
	}
}

// stationNets are the host's networks that hold the address the radio
// announces, i.e. the station LAN. Nothing is known before the first
// announcement is heard, and then no device is taken to be at the station.
func (a *Announcer) stationNets(radioIP netip.Addr) []netip.Prefix {
	if !radioIP.IsValid() {
		return nil
	}
	list := a.Interfaces
	if list == nil {
		list = interfacePrefixes
	}
	var out []netip.Prefix
	for _, p := range list() {
		if p.Contains(radioIP) {
			out = append(out, p)
		}
	}
	return out
}

func slicesEqualAddr(x, y []netip.Addr) bool {
	if len(x) != len(y) {
		return false
	}
	for i := range x {
		if x[i] != y[i] {
			return false
		}
	}
	return true
}

// AnnounceStatus is the announcement's part of /v1/status.
type AnnounceStatus struct {
	// Heard: when the radio's announcement was last heard on the LAN (RFC
	// 3339); empty if it never has been, in which case nothing is sent.
	Heard   string         `json:"heard"`
	Devices []AnnouncePeer `json:"devices"`
}

func (a *Announcer) Status() AnnounceStatus {
	a.mu.Lock()
	defer a.mu.Unlock()
	st := AnnounceStatus{Devices: append([]AnnouncePeer{}, a.report...)}
	if !a.heard.IsZero() {
		st.Heard = a.heard.UTC().Format(time.RFC3339)
	}
	return st
}
