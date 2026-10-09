package main

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"log"
	"net"
	"net/netip"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// SmartSDR ports on the radio. The radio sends VITA-49 from 4993 and accepts
// client VITA-49 (TX audio, DAX TX, netcw) on 4991; the TCP API is 4992.
const (
	radioAPIPort    = 4992
	radioVitaInPort = 4991
	radioPrimePort  = 4992
)

// Relay bridges tailnet clients to the radio. One Session per accepted
// control connection; UDP is demultiplexed by the client's tailnet address.
type Relay struct {
	RadioAddr string // SSDR_RADIO_ADDRESS, normally 172.30.1.1

	// DialRadio opens the radio API connection (net.Dial in production).
	DialRadio func() (net.Conn, error)
	// HostUDP opens the per-session UDP socket in the radio's network
	// namespace; the radio sends this session's VITA-49 to it.
	HostUDP func() (*net.UDPConn, error)

	// Tailnet-side UDP sockets: TX in (4991), primes in (4992), VITA out (4993).
	ClientTX    net.PacketConn
	ClientPrime net.PacketConn
	ClientOut   net.PacketConn

	// Authorize decides whether a tailnet peer may connect. It returns a
	// human-readable identity for logs.
	Authorize func(remote net.Addr) (string, bool)

	// MaxMTU clamps `network_mtu=` so VITA-49 fits the tunnel (0 = no clamp).
	MaxMTU int

	// Radio UDP ports; zero means the SmartSDR defaults (tests override).
	RadioTXPort, RadioPrimePort int

	// OpenForward opens a tailnet listener on port for a radio side channel
	// that only client may use (see forward.go). Nil disables side-channel
	// forwarding.
	OpenForward func(port int, client netip.Addr) error

	// LocalAddrs lists this host's own addresses (net.InterfaceAddrs when
	// nil). A session's host socket listens on every interface, but only the
	// radio may feed it: the container shares the radio's network namespace,
	// so the radio's datagrams carry one of these addresses, and the kernel
	// drops anything arriving from outside with a local source address.
	LocalAddrs func() []netip.Addr

	localMu sync.Mutex
	local   map[netip.Addr]bool
	localAt time.Time

	mu       sync.Mutex
	byClient map[netip.AddrPort]*Session // client UDP address -> session
	sessions map[uint64]*Session
	nextID   atomic.Uint64
	verbs    verbLog
}

// Session is one AetherSDR control connection relayed to the radio.
type Session struct {
	id        uint64
	relay     *Relay
	who       string
	peer      string
	client    net.Conn
	radio     net.Conn
	host      *net.UDPConn
	clientIP  netip.Addr
	udpClient atomic.Pointer[netip.AddrPort] // where VITA-49 goes on the tailnet
	closeOnce sync.Once
	done      chan struct{}

	rxPackets, txPackets atomic.Uint64

	pendingMu sync.Mutex
	pending   map[string]bool // sequence numbers of file upload/download commands

	// UDP diagnostics, reported in /v1/status and the caller's /v1/session.
	fromRadio, toClientErrs, maxDatagram atomic.Uint64
	// Datagrams on the host socket that did not come from the radio; dropped.
	fromOthers   atomic.Uint64
	lastRejected atomic.Pointer[string]
	vita         vitaStats
	lastSendErr  atomic.Pointer[string]

	// The shim's own MTU clamp, sent once after `client gui` with a sequence
	// number the client never uses; its reply is not forwarded.
	mtuInjected bool
	mtuSeq      string
}

func (r *Relay) init() {
	r.mu.Lock()
	if r.byClient == nil {
		r.byClient = map[netip.AddrPort]*Session{}
	}
	if r.sessions == nil {
		r.sessions = map[uint64]*Session{}
	}
	r.mu.Unlock()
}

// Serve accepts control connections until l is closed.
func (r *Relay) Serve(l net.Listener) error {
	r.init()
	if r.RadioTXPort == 0 {
		r.RadioTXPort = radioVitaInPort
	}
	if r.RadioPrimePort == 0 {
		r.RadioPrimePort = radioPrimePort
	}
	go r.pumpClientUDP(r.ClientTX, r.RadioTXPort, true)
	go r.pumpClientUDP(r.ClientPrime, r.RadioPrimePort, false)
	for {
		c, err := l.Accept()
		if err != nil {
			return err
		}
		go r.handle(c)
	}
}

func addrPortOf(a net.Addr) (netip.AddrPort, bool) {
	switch v := a.(type) {
	case *net.UDPAddr:
		ap := v.AddrPort()
		return netip.AddrPortFrom(ap.Addr().Unmap(), ap.Port()), true
	case *net.TCPAddr:
		ap := v.AddrPort()
		return netip.AddrPortFrom(ap.Addr().Unmap(), ap.Port()), true
	}
	ap, err := netip.ParseAddrPort(a.String())
	if err != nil {
		return netip.AddrPort{}, false
	}
	return netip.AddrPortFrom(ap.Addr().Unmap(), ap.Port()), true
}

func (r *Relay) handle(c net.Conn) {
	who, ok := "anonymous", true
	if r.Authorize != nil {
		who, ok = r.Authorize(c.RemoteAddr())
	}
	if !ok {
		log.Printf("reject control connection from %s (%s)", c.RemoteAddr(), who)
		c.Close()
		return
	}
	clientAP, okAddr := addrPortOf(c.RemoteAddr())
	if !okAddr {
		log.Printf("reject %s: unparseable address", c.RemoteAddr())
		c.Close()
		return
	}
	radio, err := r.DialRadio()
	if err != nil {
		log.Printf("radio dial failed for %s: %v", who, err)
		c.Close()
		return
	}
	host, err := r.HostUDP()
	if err != nil {
		log.Printf("host UDP socket failed for %s: %v", who, err)
		radio.Close()
		c.Close()
		return
	}
	s := &Session{
		id: r.nextID.Add(1), relay: r, who: who, peer: peerName(who), client: c, radio: radio,
		host: host, clientIP: clientAP.Addr().Unmap(), done: make(chan struct{}),
	}
	log.Printf("session %d: %s from %s -> radio %s, host UDP %s", s.id, who, c.RemoteAddr(), radio.RemoteAddr(), host.LocalAddr())
	r.mu.Lock()
	r.sessions[s.id] = s
	r.mu.Unlock()

	go s.radioToClientTCP()
	go s.clientToRadioTCP()
	go s.radioToClientUDP()
	<-s.done
	log.Printf("session %d closed (rx %d / tx %d datagrams)", s.id, s.rxPackets.Load(), s.txPackets.Load())
}

// close tears the whole session down. Closing the radio connection is what
// makes the radio drop this GUI client and unkey any transmitter it owns
// (Principle VI): the relay never keeps a radio session alive for a client
// it can no longer hear.
func (s *Session) close(reason string) {
	s.closeOnce.Do(func() {
		log.Printf("session %d: closing: %s", s.id, reason)
		s.radio.Close()
		s.client.Close()
		s.host.Close()
		s.relay.mu.Lock()
		delete(s.relay.sessions, s.id)
		for k, v := range s.relay.byClient {
			if v == s {
				delete(s.relay.byClient, k)
			}
		}
		s.relay.mu.Unlock()
		close(s.done)
	})
}

// Radio -> client control bytes are forwarded verbatim, line by line, so
// replies that announce a side-channel port can open a tailnet forwarder
// before the client acts on them.
func (s *Session) radioToClientTCP() {
	br := bufio.NewReaderSize(s.radio, 64*1024)
	for {
		line, err := readLine(br, maxControlLine)
		if len(line) > 0 {
			if port := s.sideChannelPort(line); port > 0 && s.relay.OpenForward != nil {
				if ferr := s.relay.OpenForward(port, s.clientIP); ferr != nil {
					log.Printf("session %d: cannot forward radio port %d: %v", s.id, port, ferr)
				} else {
					log.Printf("session %d: forwarding radio side channel on TCP %d", s.id, port)
				}
			}
			if ours, accepted := s.injectedReply(line); ours {
				if !accepted {
					log.Printf("session %d: radio refused the MTU clamp: %s", s.id, bytes.TrimSpace(line))
				}
				continue
			}
			if _, werr := s.client.Write(line); werr != nil {
				s.close(fmt.Sprintf("client write failed: %v", werr))
				return
			}
		}
		if err != nil {
			s.close(fmt.Sprintf("radio side ended: %v", err))
			return
		}
	}
}

var (
	fileCmdRe = regexp.MustCompile(`^C[A-Z]*(\d+)\|file (upload|download)\b`)
	replyRe   = regexp.MustCompile(`^R(\d+)\|0+\|(\d+)\s*$`)
	verbRe    = regexp.MustCompile(`^C[A-Z]*\d+\|([a-z_]+)(?: ([a-z_]+))?`)
)

// noteCommand remembers file transfer commands, whose replies name a port,
// and records the command verb for diagnostics.
func (s *Session) noteCommand(line []byte) {
	if m := verbRe.FindSubmatch(line); m != nil {
		v := string(m[1])
		if len(m[2]) > 0 {
			v += " " + string(m[2])
		}
		s.relay.verbs.add(v)
	}
	if m := fileCmdRe.FindSubmatch(line); m != nil {
		s.pendingMu.Lock()
		if s.pending == nil {
			s.pending = map[string]bool{}
		}
		s.pending[string(m[1])] = true
		s.pendingMu.Unlock()
	}
}

// sideChannelPort returns the port in a successful reply to a pending file
// transfer command (`R<seq>|0|<port>`), or 0.
func (s *Session) sideChannelPort(line []byte) int {
	m := replyRe.FindSubmatch(bytes.TrimRight(line, "\r\n"))
	if m == nil {
		return 0
	}
	s.pendingMu.Lock()
	ok := s.pending[string(m[1])]
	delete(s.pending, string(m[1]))
	s.pendingMu.Unlock()
	if !ok {
		return 0
	}
	port, err := strconv.Atoi(string(m[2]))
	if err != nil || port < 1024 || port > 65535 || port == radioAPIPort {
		return 0
	}
	return port
}

var (
	udpportRe = regexp.MustCompile(`^(C[A-Z]*\d+\|client udpport )(\d+)\s*$`)
	mtuRe     = regexp.MustCompile(`(\bnetwork_mtu=)(\d+)`)
)

// rewriteLine adjusts the two commands that carry transport details:
// `client udpport N` (the radio must send to the relay's host socket, not to
// the client's port) and `network_mtu=` (VITA-49 must fit the tunnel MTU).
// It reports the client's own UDP port when it sees `client udpport`.
func (s *Session) rewriteLine(line []byte) ([]byte, int) {
	body := bytes.TrimRight(line, "\r\n")
	ending := line[len(body):]
	if m := udpportRe.FindSubmatch(body); m != nil {
		port, err := strconv.Atoi(string(m[2]))
		if err == nil && port > 0 && port < 65536 {
			hostPort := s.host.LocalAddr().(*net.UDPAddr).Port
			out := append([]byte{}, m[1]...)
			out = strconv.AppendInt(out, int64(hostPort), 10)
			return append(out, ending...), port
		}
	}
	if s.relay.MaxMTU > 0 && bytes.Contains(body, []byte("network_mtu=")) {
		out := mtuRe.ReplaceAllFunc(body, func(m []byte) []byte {
			sm := mtuRe.FindSubmatch(m)
			v, err := strconv.Atoi(string(sm[2]))
			if err != nil || v <= s.relay.MaxMTU {
				return m
			}
			return append(append([]byte{}, sm[1]...), strconv.Itoa(s.relay.MaxMTU)...)
		})
		return append(out, ending...), 0
	}
	return line, 0
}

// maxControlLine bounds one SmartSDR control line in either direction. The
// longest real ones (radio status and list replies) are a few KB; the cap
// only stops a peer from making the shim, which shares the radio's memory
// and has no container limit, buffer an endless line.
const maxControlLine = 1 << 20

var errLineTooLong = errors.New("control line longer than 1 MiB")

// readLine reads through the next '\n' like ReadBytes, but fails with
// errLineTooLong instead of growing past max. A final line without '\n' is
// returned with the reader's error, as ReadBytes does.
func readLine(br *bufio.Reader, max int) ([]byte, error) {
	var line []byte
	for {
		frag, err := br.ReadSlice('\n')
		if len(line)+len(frag) > max {
			return nil, errLineTooLong
		}
		line = append(line, frag...)
		if err != bufio.ErrBufferFull {
			return line, err
		}
	}
}

// Client -> radio control text, line by line so the two transport commands
// can be rewritten. All other bytes pass through unchanged.
func (s *Session) clientToRadioTCP() {
	br := bufio.NewReaderSize(s.client, 64*1024)
	for {
		line, err := readLine(br, maxControlLine)
		if len(line) > 0 {
			s.noteCommand(line)
			out, clientPort := s.rewriteLine(line)
			if clientPort > 0 {
				s.bindClientUDP(netip.AddrPortFrom(s.clientIP, uint16(clientPort)))
			}
			if _, werr := s.radio.Write(out); werr != nil {
				s.close(fmt.Sprintf("radio write failed: %v", werr))
				return
			}
			if inj := s.mtuInjection(line); inj != nil {
				if _, werr := s.radio.Write(inj); werr != nil {
					s.close(fmt.Sprintf("radio write failed: %v", werr))
					return
				}
			}
		}
		if err != nil {
			s.close(fmt.Sprintf("client side ended: %v", err))
			return
		}
	}
}

var clientGuiRe = regexp.MustCompile(`^C[A-Z]*\d+\|client gui\b`)

// mtuInjection returns the shim's own `client set network_mtu=` command to
// send right after the client's `client gui`, so VITA-49 fits the tunnel even
// when the client (SmartSDR, for one) never sets an MTU itself.
func (s *Session) mtuInjection(line []byte) []byte {
	if s.mtuInjected || s.relay.MaxMTU <= 0 || !clientGuiRe.Match(line) {
		return nil
	}
	s.mtuInjected = true
	s.pendingMu.Lock()
	s.mtuSeq = strconv.FormatUint(990000000+s.id, 10)
	s.pendingMu.Unlock()
	log.Printf("session %d: clamping radio network MTU to %d", s.id, s.relay.MaxMTU)
	return []byte(fmt.Sprintf("C%s|client set enforce_network_mtu=1 network_mtu=%d\n", s.mtuSeq, s.relay.MaxMTU))
}

// injectedReply reports whether line answers the shim's own command, and if
// so whether the radio accepted it.
func (s *Session) injectedReply(line []byte) (ours, accepted bool) {
	s.pendingMu.Lock()
	seq := s.mtuSeq
	s.pendingMu.Unlock()
	if seq == "" || !bytes.HasPrefix(line, []byte("R"+seq+"|")) {
		return false, false
	}
	m := regexp.MustCompile(`^R\d+\|0+\|`).Match(line)
	return true, m
}

// bindClientUDP points the session's UDP at ap. A repeated `client udpport`
// (AetherSDR retries after a collision) replaces the earlier address, which
// would otherwise keep relaying that port's datagrams into this session.
func (s *Session) bindClientUDP(ap netip.AddrPort) {
	prev := s.udpClient.Swap(&ap)
	s.relay.mu.Lock()
	if prev != nil && *prev != ap && s.relay.byClient[*prev] == s {
		delete(s.relay.byClient, *prev)
	}
	s.relay.byClient[ap] = s
	s.relay.mu.Unlock()
	log.Printf("session %d: client UDP %s <-> host %s", s.id, ap, s.host.LocalAddr())
}

// Radio -> client VITA-49: everything the radio sends to this session's host
// socket goes to the client's registered tailnet address. Datagrams from any
// other sender (a LAN host that found the port) are dropped and counted.
func (s *Session) radioToClientUDP() {
	buf := make([]byte, 65536)
	for {
		n, src, err := s.host.ReadFromUDPAddrPort(buf)
		if err != nil {
			s.close(fmt.Sprintf("host UDP ended: %v", err))
			return
		}
		if !s.relay.fromRadio(src.Addr()) {
			from := src.String()
			s.lastRejected.Store(&from)
			if s.fromOthers.Add(1) == 1 {
				log.Printf("session %d: dropping datagrams from %s: not the radio", s.id, from)
			}
			continue
		}
		s.fromRadio.Add(1)
		s.vita.note(buf[:n])
		if uint64(n) > s.maxDatagram.Load() {
			s.maxDatagram.Store(uint64(n))
		}
		dst := s.udpClient.Load()
		if dst == nil {
			continue // radio is early; the client hasn't registered yet
		}
		if _, err := s.relay.ClientOut.WriteTo(buf[:n], net.UDPAddrFromAddrPort(*dst)); err == nil {
			s.rxPackets.Add(1)
		} else {
			s.toClientErrs.Add(1)
			msg := err.Error()
			s.lastSendErr.Store(&msg)
		}
	}
}

// fromRadio reports whether a host-socket datagram's source is the radio:
// RadioAddr or one of this host's own addresses. The address list is
// re-read at most once a second, when an unknown source arrives.
func (r *Relay) fromRadio(a netip.Addr) bool {
	a = a.Unmap()
	if ra, err := netip.ParseAddr(r.RadioAddr); err == nil && ra.Unmap() == a {
		return true
	}
	r.localMu.Lock()
	defer r.localMu.Unlock()
	if r.local == nil || (!r.local[a] && time.Since(r.localAt) > time.Second) {
		list := r.LocalAddrs
		if list == nil {
			list = interfaceAddrs
		}
		r.local = map[netip.Addr]bool{}
		for _, l := range list() {
			r.local[l.Unmap()] = true
		}
		r.localAt = time.Now()
	}
	return r.local[a]
}

func interfaceAddrs() []netip.Addr {
	as, err := net.InterfaceAddrs()
	if err != nil {
		log.Printf("listing local addresses: %v", err)
		return nil
	}
	out := make([]netip.Addr, 0, len(as))
	for _, a := range as {
		if p, ok := a.(*net.IPNet); ok {
			if ip, ok := netip.AddrFromSlice(p.IP); ok {
				out = append(out, ip.Unmap())
			}
		}
	}
	return out
}

// Client -> radio datagrams (TX VITA-49 on 4991, primes on 4992) leave from
// the session's host socket, so the radio sees one address per client, as it
// would on a LAN.
func (r *Relay) pumpClientUDP(pc net.PacketConn, radioPort int, countTX bool) {
	if pc == nil {
		return
	}
	buf := make([]byte, 65536)
	dst := &net.UDPAddr{IP: net.ParseIP(r.RadioAddr), Port: radioPort}
	for {
		n, src, err := pc.ReadFrom(buf)
		if err != nil {
			log.Printf("tailnet UDP %d ended: %v", radioPort, err)
			return
		}
		ap, ok := addrPortOf(src)
		if !ok {
			continue
		}
		r.mu.Lock()
		s := r.byClient[ap]
		r.mu.Unlock()
		if s == nil {
			continue // unknown sender: drop (Principle VII)
		}
		if _, err := s.host.WriteToUDP(buf[:n], dst); err == nil && countTX {
			s.txPackets.Add(1)
		}
	}
}

// waitClosed is used by tests.
func (s *Session) waitClosed(d time.Duration) bool {
	select {
	case <-s.done:
		return true
	case <-time.After(d):
		return false
	}
}

// RecentCommands reports recently seen command verbs (no arguments).
func (r *Relay) RecentCommands() []string { return r.verbs.snapshot() }

// SessionInfo is one relayed session's UDP picture, for /v1/status.
type SessionInfo struct {
	ID            uint64 `json:"id"`
	Peer          string `json:"peer"` // tailnet machine name, never a login
	ClientUDP     string `json:"client_udp"`
	FromRadio     uint64 `json:"from_radio"`
	ToClient      uint64 `json:"to_client"`
	ToClientFails uint64 `json:"to_client_failures"`
	FromClient    uint64 `json:"from_client"`
	MaxDatagram   uint64 `json:"max_datagram"`
	LastSendError string `json:"last_send_error"`
	// Datagrams on the host socket from anything but the radio, dropped.
	// Nonzero on a healthy session would mean the radio's own source
	// address isn't recognised, so its VITA-49 is being dropped too.
	FromOthers   uint64 `json:"from_other_sources"`
	LastRejected string `json:"last_rejected_source"`
	// Per-stream packets and sequence gaps as datagrams leave the radio,
	// before the tunnel.
	Streams []StreamInfo `json:"streams"`

	clientIP netip.Addr // matches /v1/session callers; never serialized
}

// Sessions reports every relayed session's UDP counters.
func (r *Relay) Sessions() []SessionInfo {
	r.mu.Lock()
	all := make([]*Session, 0, len(r.sessions))
	for _, s := range r.sessions {
		all = append(all, s)
	}
	r.mu.Unlock()
	out := make([]SessionInfo, 0, len(all))
	for _, s := range all {
		info := SessionInfo{
			ID: s.id, Peer: s.peer, FromRadio: s.fromRadio.Load(), ToClient: s.rxPackets.Load(),
			ToClientFails: s.toClientErrs.Load(), FromClient: s.txPackets.Load(),
			MaxDatagram: s.maxDatagram.Load(),
			Streams:     s.vita.snapshot(),
			clientIP:    s.clientIP,
		}
		if ap := s.udpClient.Load(); ap != nil {
			info.ClientUDP = ap.String()
		}
		if e := s.lastSendErr.Load(); e != nil {
			info.LastSendError = *e
		}
		info.FromOthers = s.fromOthers.Load()
		if e := s.lastRejected.Load(); e != nil {
			info.LastRejected = *e
		}
		out = append(out, info)
	}
	return out
}

// SessionCount reports how many AetherSDR sessions are being relayed.
func (r *Relay) SessionCount() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return len(r.sessions)
}

// CloseAll ends every session; each one closes its radio connection, so the
// radio drops those clients (Principle VI). Used when the tailnet goes away.
func (r *Relay) CloseAll(reason string) {
	r.mu.Lock()
	all := make([]*Session, 0, len(r.sessions))
	for _, s := range r.sessions {
		all = append(all, s)
	}
	r.mu.Unlock()
	for _, s := range all {
		s.close(reason)
	}
}

// peerName drops the login from "login on machine", keeping the machine name.
func peerName(who string) string {
	if i := strings.LastIndex(who, " on "); i >= 0 {
		return who[i+len(" on "):]
	}
	return who
}
