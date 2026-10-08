package main

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"net/netip"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strings"
	"sync"
	"time"

	"tailscale.com/client/local"
	"tailscale.com/ipn"
	"tailscale.com/tsnet"
)

// Node states reported to AetherSDR.
const (
	stateUnprovisioned = "unprovisioned" // no key has ever been accepted
	stateStarting      = "starting"      // joining the tailnet
	stateRunning       = "running"       // on the tailnet, relaying
	stateError         = "error"         // last attempt failed; see LastError
)

// provisionConfig is persisted in the state directory. It never holds the
// Tailscale auth key: tsnet spends the key on first login and keeps its own
// node credentials. Only a hash of the admin token is stored.
type provisionConfig struct {
	TokenSHA256 string   `json:"token_sha256"`
	Hostname    string   `json:"hostname"`
	Allow       []string `json:"allow"`
	// LAN prefixes advertised as Tailscale subnet routes, e.g. the station's
	// Antenna Genius and Tuner Genius XL. tsnet's netstack forwards routed
	// flows out through the radio's own network.
	Routes []string `json:"routes"`
	// ShareDiscovered shares every 4O3A device the container hears on the LAN
	// (nil means the default: on).
	ShareDiscovered *bool `json:"share_discovered,omitempty"`
}

func (c provisionConfig) shareDiscovered() bool {
	return c.ShareDiscovered == nil || *c.ShareDiscovered
}

// Node owns the tailnet membership and the relay that runs on it. Keys are
// swapped in-process: nothing is known to restart a container whose process
// exits, so the shim never relies on exiting to reconfigure itself.
type Node struct {
	StateDir  string
	RadioAddr string
	MaxMTU    int
	Discovery *Discovery

	opMu sync.Mutex // serializes provision / sign-out / start

	mu         sync.Mutex
	cfg        provisionConfig
	state      string
	lastError  string
	srv        *tsnet.Server
	relay      *Relay
	lc         *local.Client
	closers    []func() error
	ip         netip.Addr
	dnsName    string
	advertised []netip.Prefix
	// Routes a remote connection has actually travelled through. Peers only
	// send traffic for a route once the tailnet has approved it, so this is
	// first-hand evidence of approval (a node's own status doesn't carry its
	// approved routes).
	routesInUse map[netip.Prefix]bool

	telemetry linkTelemetry // per-peer path, RTT and throughput (telemetry.go)
}

func (n *Node) cfgPath() string   { return filepath.Join(n.StateDir, "provision.json") }
func (n *Node) tsnetDir() string  { return filepath.Join(n.StateDir, "tsnet") }
func (n *Node) provisioned() bool { return n.cfg.TokenSHA256 != "" }

func (n *Node) load() error {
	if err := os.MkdirAll(n.StateDir, 0o700); err != nil {
		return err
	}
	b, err := os.ReadFile(n.cfgPath())
	if errors.Is(err, os.ErrNotExist) {
		n.state = stateUnprovisioned
		return nil
	}
	if err != nil {
		return err
	}
	if err := json.Unmarshal(b, &n.cfg); err != nil {
		return fmt.Errorf("corrupt %s: %w", n.cfgPath(), err)
	}
	if n.provisioned() {
		n.state = stateStarting
	} else {
		n.state = stateUnprovisioned
	}
	return nil
}

func (n *Node) save(cfg provisionConfig) error {
	b, _ := json.MarshalIndent(cfg, "", "  ")
	tmp := n.cfgPath() + ".tmp"
	if err := os.WriteFile(tmp, b, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, n.cfgPath())
}

func (n *Node) setState(state, lastErr string) {
	n.mu.Lock()
	n.state, n.lastError = state, lastErr
	n.mu.Unlock()
}

// Boot resumes a provisioned node after a restart or radio reboot. The
// container starts before the radio API listens and before NTP sets the
// clock, so it keeps retrying in the background.
func (n *Node) Boot() {
	n.mu.Lock()
	provisioned := n.provisioned()
	n.mu.Unlock()
	if !provisioned {
		log.Printf("unprovisioned: waiting for AetherSDR to supply a Tailscale auth key")
		return
	}
	go func() {
		for attempt := 1; ; attempt++ {
			n.opMu.Lock()
			n.mu.Lock()
			still := n.provisioned() && n.srv == nil
			n.mu.Unlock()
			if !still {
				n.opMu.Unlock()
				return
			}
			err := n.start("", 2*time.Minute)
			n.opMu.Unlock()
			if err == nil {
				return
			}
			log.Printf("tailnet start attempt %d failed: %v", attempt, err)
			time.Sleep(min(time.Duration(attempt)*10*time.Second, 2*time.Minute))
		}
	}()
}

// start brings the tailnet up and the relay on it. Caller holds opMu.
func (n *Node) start(authKey string, timeout time.Duration) error {
	n.mu.Lock()
	cfg := n.cfg
	n.mu.Unlock()
	n.setState(stateStarting, "")

	srv := &tsnet.Server{
		Hostname: cfg.Hostname,
		Dir:      n.tsnetDir(),
		AuthKey:  authKey,
		Logf:     func(string, ...any) {},
		UserLogf: log.Printf,
	}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	st, err := srv.Up(ctx)
	cancel()
	if err != nil {
		srv.Close()
		n.setState(stateError, err.Error())
		return err
	}
	var ip4 netip.Addr
	for _, ip := range st.TailscaleIPs {
		if ip.Is4() {
			ip4 = ip
		}
	}
	if !ip4.IsValid() {
		srv.Close()
		n.setState(stateError, "no tailnet IPv4 address")
		return errors.New("no tailnet IPv4 address")
	}
	lc, err := srv.LocalClient()
	if err != nil {
		srv.Close()
		n.setState(stateError, err.Error())
		return err
	}

	var closers []func() error
	fail := func(err error) error {
		for _, c := range closers {
			c()
		}
		srv.Close()
		n.setState(stateError, err.Error())
		return err
	}
	listenUDP := func(port int) (net.PacketConn, error) {
		pc, err := srv.ListenPacket("udp4", fmt.Sprintf("%s:%d", ip4, port))
		if err == nil {
			closers = append(closers, pc.Close)
		}
		return pc, err
	}
	tx, err := listenUDP(radioVitaInPort)
	if err != nil {
		return fail(err)
	}
	prime, err := listenUDP(radioPrimePort)
	if err != nil {
		return fail(err)
	}
	out, err := listenUDP(4993)
	if err != nil {
		return fail(err)
	}
	ln, err := srv.Listen("tcp", fmt.Sprintf(":%d", radioAPIPort))
	if err != nil {
		return fail(err)
	}
	closers = append(closers, ln.Close)

	relay := &Relay{
		RadioAddr: n.RadioAddr,
		DialRadio: func() (net.Conn, error) {
			return net.DialTimeout("tcp", net.JoinHostPort(n.RadioAddr, fmt.Sprint(radioAPIPort)), 5*time.Second)
		},
		HostUDP: func() (*net.UDPConn, error) {
			return net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero})
		},
		ClientTX: tx, ClientPrime: prime, ClientOut: out,
		Authorize: n.authorizer(lc),
		MaxMTU:    n.MaxMTU,
	}
	fwd := &Forwarder{
		Listen: func(port int) (net.Listener, error) {
			return srv.Listen("tcp", fmt.Sprintf(":%d", port))
		},
		DialRadio: func(port int) (net.Conn, error) {
			return net.DialTimeout("tcp", net.JoinHostPort(n.RadioAddr, fmt.Sprint(port)), 5*time.Second)
		},
		Authorize: n.authorizer(lc),
	}
	relay.OpenForward = fwd.Open
	go func() {
		err := relay.Serve(ln)
		log.Printf("relay stopped: %v", err)
	}()

	// tsnet drops TCP flows that no listener claims, including flows to our
	// own advertised subnet routes. Forward those out through the radio's
	// network; the tailnet's ACLs have already admitted the sender.
	unregister := srv.RegisterFallbackTCPHandler(n.subnetTCP)
	closers = append(closers, func() error { unregister(); return nil })

	// Link telemetry for AetherSDR's Network Diagnostics: tailnet-only, and
	// each caller sees only its own sessions.
	tl, err := srv.Listen("tcp", fmt.Sprintf(":%d", TelemetryPort))
	if err != nil {
		return fail(err)
	}
	closers = append(closers, tl.Close)
	go http.Serve(tl, n.telemetryHandler(n.authorizer(lc)))
	tctx, tcancel := context.WithCancel(context.Background())
	closers = append(closers, func() error { tcancel(); return nil })
	go n.runTelemetry(tctx, lc)

	n.mu.Lock()
	n.srv, n.relay, n.closers, n.ip = srv, relay, closers, ip4
	n.dnsName = strings.TrimSuffix(st.Self.DNSName, ".")
	n.state, n.lastError = stateRunning, ""
	n.mu.Unlock()
	log.Printf("tailnet node %q up at %s (%s)", cfg.Hostname, ip4, n.dnsName)
	n.mu.Lock()
	n.lc = lc
	n.advertised = nil
	n.mu.Unlock()
	n.RefreshRoutes()
	go reportHealth(lc)
	return nil
}

// effectiveRoutes is the operator's extra devices plus, when sharing is on,
// every discovered station device as a /32.
func (n *Node) effectiveRoutes(cfg provisionConfig) []netip.Prefix {
	seen := map[netip.Prefix]bool{}
	var out []netip.Prefix
	add := func(p netip.Prefix) {
		if !seen[p] {
			seen[p] = true
			out = append(out, p)
		}
	}
	for _, r := range cfg.Routes {
		if p, err := parseRoute(r); err == nil {
			add(p)
		}
	}
	if cfg.shareDiscovered() && n.Discovery != nil {
		for _, d := range n.Discovery.Devices() {
			if p, err := parseRoute(d.IP); err == nil {
				add(p)
			}
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].String() < out[j].String() })
	return out
}

// RefreshRoutes advertises the current effective routes if they changed.
// They take effect once approved in the tailnet's admin console (or by an
// autoApprovers rule).
func (n *Node) RefreshRoutes() {
	n.mu.Lock()
	lc, cfg, prev := n.lc, n.cfg, n.advertised
	n.mu.Unlock()
	if lc == nil {
		return
	}
	want := n.effectiveRoutes(cfg)
	if slices.Equal(want, prev) && prev != nil {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if _, err := lc.EditPrefs(ctx, &ipn.MaskedPrefs{
		Prefs:              ipn.Prefs{AdvertiseRoutes: want},
		AdvertiseRoutesSet: true,
	}); err != nil {
		log.Printf("advertising LAN routes %v: %v", want, err)
		return
	}
	if want == nil {
		want = []netip.Prefix{}
	}
	n.mu.Lock()
	n.advertised = want
	n.mu.Unlock()
	log.Printf("advertising LAN routes %v", want)
}

// SetSharing changes which LAN devices are shared, without leaving the
// tailnet. shareDiscovered nil leaves that setting unchanged.
func (n *Node) SetSharing(routes []string, shareDiscovered *bool) error {
	n.opMu.Lock()
	n.mu.Lock()
	cfg := n.cfg
	n.mu.Unlock()
	cfg.Routes = routes
	if shareDiscovered != nil {
		cfg.ShareDiscovered = shareDiscovered
	}
	if err := n.save(cfg); err != nil {
		n.opMu.Unlock()
		return err
	}
	n.mu.Lock()
	n.cfg = cfg
	n.mu.Unlock()
	n.opMu.Unlock()
	n.RefreshRoutes()
	return nil
}

// stop leaves the relay and tailnet. With logout it also deregisters the
// node, so the next start needs a new auth key. Caller holds opMu.
func (n *Node) stop(logout bool) {
	n.mu.Lock()
	srv, relay, closers := n.srv, n.relay, n.closers
	n.srv, n.relay, n.closers, n.ip, n.dnsName, n.lc = nil, nil, nil, netip.Addr{}, "", nil
	n.routesInUse = nil
	n.telemetry.mu.Lock()
	n.telemetry.peers = nil
	n.telemetry.mu.Unlock()
	n.mu.Unlock()
	if relay != nil {
		relay.CloseAll("tailnet node stopping")
	}
	for _, c := range closers {
		c()
	}
	if srv != nil {
		if logout {
			if lc, err := srv.LocalClient(); err == nil {
				ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
				if err := lc.Logout(ctx); err != nil {
					log.Printf("logout: %v", err)
				}
				cancel()
			}
		}
		srv.Close()
	}
	if logout {
		os.RemoveAll(n.tsnetDir())
	}
}

// authorizer checks each tailnet caller with WhoIs against the current
// allowlist (logins or tags). An empty allowlist admits any tailnet peer.
func (n *Node) authorizer(lc *local.Client) func(net.Addr) (string, bool) {
	return func(remote net.Addr) (string, bool) {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		who, err := lc.WhoIs(ctx, remote.String())
		if err != nil {
			return "unknown peer: " + err.Error(), false
		}
		id := who.Node.ComputedName
		if who.UserProfile != nil && !who.Node.IsTagged() {
			id = who.UserProfile.LoginName + " on " + id
		}
		n.mu.Lock()
		allow := append([]string{}, n.cfg.Allow...)
		n.mu.Unlock()
		if len(allow) == 0 {
			return id, true
		}
		for _, a := range allow {
			if who.UserProfile != nil && !who.Node.IsTagged() && a == who.UserProfile.LoginName {
				return id, true
			}
			for _, t := range who.Node.Tags {
				if a == t {
					return id, true
				}
			}
		}
		return id, false
	}
}

// checkToken compares a presented admin token with the stored hash.
func (n *Node) checkToken(presented string) bool {
	n.mu.Lock()
	want := n.cfg.TokenSHA256
	n.mu.Unlock()
	if want == "" || presented == "" {
		return false
	}
	return subtle.ConstantTimeCompare([]byte(sha256Hex(presented)), []byte(want)) == 1
}

func sha256Hex(s string) string {
	sum := sha256.Sum256([]byte(s))
	return hex.EncodeToString(sum[:])
}

func newToken() (token, hash string) {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	token = base64.RawURLEncoding.EncodeToString(b)
	return token, sha256Hex(token)
}

// Provision joins (or re-joins) the tailnet with a fresh auth key. The first
// call needs no token (trust on first use, matching the radio API's own LAN
// trust); later calls must present the admin token returned by the first.
// It returns a new admin token on success.
func (n *Node) Provision(authKey, hostname string, allow, routes []string, shareDiscovered *bool) (string, error) {
	n.opMu.Lock()
	defer n.opMu.Unlock()

	n.stop(true)
	token, hash := newToken()
	cfg := provisionConfig{TokenSHA256: hash, Hostname: hostname, Allow: allow, Routes: routes,
		ShareDiscovered: shareDiscovered}
	n.mu.Lock()
	n.cfg = cfg
	n.mu.Unlock()

	if err := n.start(authKey, 90*time.Second); err != nil {
		// A rejected key leaves the node unprovisioned rather than half-joined.
		n.stop(true)
		n.mu.Lock()
		n.cfg = provisionConfig{}
		n.state = stateError
		n.mu.Unlock()
		os.Remove(n.cfgPath())
		return "", err
	}
	if err := n.save(cfg); err != nil {
		return "", fmt.Errorf("joined the tailnet but could not save settings: %w", err)
	}
	return token, nil
}

// SetAllow changes who may connect, without leaving the tailnet.
func (n *Node) SetAllow(allow []string) error {
	n.opMu.Lock()
	defer n.opMu.Unlock()
	n.mu.Lock()
	cfg := n.cfg
	cfg.Allow = allow
	n.mu.Unlock()
	if err := n.save(cfg); err != nil {
		return err
	}
	n.mu.Lock()
	n.cfg = cfg
	n.mu.Unlock()
	return nil
}

// SignOut leaves the tailnet, deregisters the node and forgets the admin
// token, returning the shim to its unprovisioned state.
func (n *Node) SignOut() {
	n.opMu.Lock()
	defer n.opMu.Unlock()
	n.stop(true)
	n.mu.Lock()
	n.cfg = provisionConfig{}
	n.state, n.lastError = stateUnprovisioned, ""
	n.mu.Unlock()
	os.Remove(n.cfgPath())
}

// Status is what GET /v1/status returns. It never contains a secret.
type Status struct {
	Version     string   `json:"version"`
	Provisioned bool     `json:"provisioned"`
	State       string   `json:"state"`
	Hostname    string   `json:"hostname"`
	TailnetIP   string   `json:"tailnet_ip"`
	DNSName     string   `json:"dns_name"`
	Allow       []string `json:"allow"`
	Sessions    int      `json:"sessions"`
	Routes      []string `json:"routes"` // extra LAN devices the operator listed
	// ShareDiscovered: share every discovered station device automatically.
	ShareDiscovered bool     `json:"share_discovered"`
	Discovered      []Device `json:"discovered_devices"`
	Advertised      []string `json:"advertised_routes"` // what the tailnet is offered now
	// Approved: advertised routes the tailnet's admin console (or an
	// autoApprovers rule) has approved, so peers can actually use them.
	Approved []string `json:"approved_routes"`
	// Per-session UDP counters: where VITA-49 stops, if it does.
	SessionDetail []SessionInfo `json:"session_detail"`
	LastError     string        `json:"last_error"`
	// Command verbs relayed so far (never arguments), for diagnosing what a
	// client such as a Maestro asks the radio for.
	RecentCommands []string `json:"recent_commands"`
}

func (n *Node) Status() Status {
	st := n.statusLocked()
	st.Approved = n.approvedRoutes()
	return st
}

// approvedRoutes asks the local tailnet backend which of this node's routes
// the control plane has approved (they appear in its AllowedIPs; PrimaryRoutes
// marks the ones it is currently routing).
func (n *Node) approvedRoutes() []string {
	n.mu.Lock()
	lc, ip := n.lc, n.ip
	out := []string{}
	seen := map[string]bool{}
	for p := range n.routesInUse {
		seen[p.String()] = true
		out = append(out, p.String())
	}
	n.mu.Unlock()
	if lc == nil {
		sort.Strings(out)
		return out
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	st, err := lc.Status(ctx)
	if err != nil || st.Self == nil {
		sort.Strings(out)
		return out
	}
	add := func(p netip.Prefix) {
		if (p.Bits() == p.Addr().BitLen() && p.Addr() == ip) || p.Addr().Is6() && p.Bits() == 128 {
			return // the node's own tailnet addresses
		}
		if !seen[p.String()] {
			seen[p.String()] = true
			out = append(out, p.String())
		}
	}
	if st.Self.PrimaryRoutes != nil {
		for _, p := range st.Self.PrimaryRoutes.All() {
			add(p)
		}
	}
	if st.Self.AllowedIPs != nil {
		for _, p := range st.Self.AllowedIPs.All() {
			if !containsTailscaleIP(st.TailscaleIPs, p) {
				add(p)
			}
		}
	}
	sort.Strings(out)
	return out
}

func containsTailscaleIP(ips []netip.Addr, p netip.Prefix) bool {
	for _, a := range ips {
		if p.Bits() == a.BitLen() && p.Addr() == a {
			return true
		}
	}
	return false
}

func (n *Node) statusLocked() Status {
	n.mu.Lock()
	defer n.mu.Unlock()
	st := Status{
		Version: shimVersion, Provisioned: n.provisioned(), State: n.state,
		Hostname: n.cfg.Hostname, DNSName: n.dnsName, Allow: n.cfg.Allow,
		LastError: n.lastError,
	}
	if st.Allow == nil {
		st.Allow = []string{}
	}
	if n.ip.IsValid() {
		st.TailnetIP = n.ip.String()
	}
	st.RecentCommands = []string{}
	st.SessionDetail = []SessionInfo{}
	st.Routes = append([]string{}, n.cfg.Routes...)
	st.ShareDiscovered = n.cfg.shareDiscovered()
	st.Discovered = []Device{}
	if n.Discovery != nil {
		st.Discovered = n.Discovery.Devices()
	}
	st.Advertised = []string{}
	for _, p := range n.advertised {
		st.Advertised = append(st.Advertised, p.String())
	}
	if n.relay != nil {
		st.SessionDetail = n.relay.Sessions()
		st.Sessions = n.relay.SessionCount()
		st.RecentCommands = n.relay.RecentCommands()
	}
	return st
}

var (
	containerNet = netip.MustParsePrefix("172.30.0.0/16")
	privateNets  = []netip.Prefix{
		netip.MustParsePrefix("10.0.0.0/8"),
		netip.MustParsePrefix("172.16.0.0/12"),
		netip.MustParsePrefix("192.168.0.0/16"),
	}
)

// parseRoute accepts a LAN address ("192.168.50.103") or prefix
// ("192.168.50.96/28"). Only private IPv4 LAN space qualifies: never a
// default route (that would be an exit node), the radio's container network,
// or anything wider than a /16.
func parseRoute(s string) (netip.Prefix, error) {
	var p netip.Prefix
	if a, err := netip.ParseAddr(s); err == nil {
		p = netip.PrefixFrom(a, a.BitLen())
	} else if pp, err := netip.ParsePrefix(s); err == nil {
		p = pp.Masked()
	} else {
		return netip.Prefix{}, fmt.Errorf("%q is not an address or prefix", s)
	}
	if !p.Addr().Is4() || p.Bits() < 16 {
		return netip.Prefix{}, fmt.Errorf("%q: only IPv4 LAN prefixes of /16 or narrower", s)
	}
	if containerNet.Overlaps(p) {
		return netip.Prefix{}, fmt.Errorf("%q overlaps the radio's container network", s)
	}
	for _, pn := range privateNets {
		if pn.Contains(p.Addr()) && pn.Bits() <= p.Bits() {
			return p, nil
		}
	}
	return netip.Prefix{}, fmt.Errorf("%q is not private LAN space", s)
}

// subnetTCP handles a TCP flow addressed to one of this node's advertised
// subnet routes (a station device): dial it from the radio's network and
// splice. Flows to anything else are left alone.
func (n *Node) subnetTCP(src, dst netip.AddrPort) (handler func(net.Conn), intercept bool) {
	n.mu.Lock()
	var route netip.Prefix
	for _, p := range n.advertised {
		if p.Contains(dst.Addr()) {
			route = p
			break
		}
	}
	n.mu.Unlock()
	if !route.IsValid() {
		return nil, false
	}
	return func(c net.Conn) {
		defer c.Close()
		n.mu.Lock()
		if n.routesInUse == nil {
			n.routesInUse = map[netip.Prefix]bool{}
		}
		n.routesInUse[route] = true
		n.mu.Unlock()
		r, err := net.DialTimeout("tcp", dst.String(), 5*time.Second)
		if err != nil {
			log.Printf("subnet %s -> %s: %v", src, dst, err)
			return
		}
		defer r.Close()
		log.Printf("subnet %s -> %s: connected", src, dst)
		done := make(chan struct{}, 2)
		go func() { io.Copy(r, c); closeWrite(r); done <- struct{}{} }()
		go func() { io.Copy(c, r); closeWrite(c); done <- struct{}{} }()
		<-done
		<-done
	}, true
}
