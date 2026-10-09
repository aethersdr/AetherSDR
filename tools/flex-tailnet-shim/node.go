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
	// The background boot loop, while one runs (under mu). Operator requests
	// cancel it rather than queue behind a tailnet login that can take
	// minutes.
	bootCancel context.CancelFunc
	bootDone   chan struct{}
	// interruptWait overrides bootInterruptWait (tests).
	interruptWait time.Duration
	// bootUnwinding is a cancelled attempt still shutting down after a
	// request gave up waiting on it (errBusy).
	bootUnwinding chan struct{}
	// routeMu serializes RefreshRoutes, so a refresh that read an older
	// configuration can never apply it after a newer one.
	routeMu sync.Mutex

	// Seams for socket-free tests: nil means the real tailnet. startFn
	// replaces start; applyRoutes replaces advertising through tsnet.
	startFn     func(ctx context.Context, authKey string, timeout time.Duration) error
	applyRoutes func([]netip.Prefix) error

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

	// authorize is the allowlist check for station-device flows, set while
	// the node runs. Nil refuses every flow. check is the same verdict with
	// "couldn't tell" kept apart, for revocation; nil means authorize's
	// answers are all definite (tests).
	authorize func(net.Addr) (string, bool)
	check     identityCheck
	// localAddrs lists the radio's own addresses (interfaceAddrs when nil);
	// a test seam. locals caches them.
	localAddrs func() []netip.Addr
	locals     localAddrCache

	fwd     *Forwarder  // side channels, while the node runs
	splices liveSplices // station-device splices in progress, for revocation
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
// clock, so it keeps retrying in the background. An attempt holds opMu while
// it waits for the tailnet, which can take minutes (a node removed in the
// admin console waits for a login that never comes), so operator requests
// interrupt it with interruptBoot instead of waiting their turn.
func (n *Node) Boot() {
	n.mu.Lock()
	provisioned := n.provisioned()
	if !provisioned || n.bootCancel != nil {
		n.mu.Unlock()
		if !provisioned {
			log.Printf("unprovisioned: waiting for AetherSDR to supply a Tailscale auth key")
		}
		return
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	n.bootCancel, n.bootDone = cancel, done
	n.mu.Unlock()
	start := n.startFn
	if start == nil {
		start = n.start
	}
	go func() {
		defer close(done)
		defer func() {
			n.mu.Lock()
			if n.bootDone == done {
				n.bootCancel, n.bootDone = nil, nil
			}
			n.mu.Unlock()
			cancel()
		}()
		for attempt := 1; ; attempt++ {
			n.opMu.Lock()
			n.mu.Lock()
			still := n.provisioned() && n.srv == nil && n.state != stateRunning && ctx.Err() == nil
			n.mu.Unlock()
			if !still {
				n.opMu.Unlock()
				return
			}
			err := start(ctx, "", 2*time.Minute)
			n.opMu.Unlock()
			if err == nil || ctx.Err() != nil {
				return
			}
			log.Printf("tailnet start attempt %d failed: %v", attempt, err)
			select {
			case <-ctx.Done():
				return
			case <-time.After(min(time.Duration(attempt)*10*time.Second, 2*time.Minute)):
			}
		}
	}()
}

// bootInterruptWait bounds how long an operator request waits for a
// cancelled boot attempt to let go, inside AetherSDR's 8 s request timeout.
const bootInterruptWait = 5 * time.Second

// errBusy: a cancelled boot attempt is still shutting its tailnet down.
var errBusy = errors.New("the tailnet connection is still shutting down; try again in a moment")

// interruptBoot cancels a running boot attempt and waits until it has let go
// of opMu. Call it before taking opMu; it returns whether a boot was running,
// so a request that leaves the node provisioned can call Boot again. If the
// attempt takes longer than the wait to unwind (tsnet's teardown is not
// ours to bound), it returns errBusy rather than outlast the caller, and
// boot resumes by itself once the attempt is gone.
func (n *Node) interruptBoot() (bool, error) {
	n.mu.Lock()
	cancel, done, unwinding := n.bootCancel, n.bootDone, n.bootUnwinding
	n.bootCancel, n.bootDone = nil, nil
	wait := n.interruptWait
	n.mu.Unlock()
	if wait <= 0 {
		wait = bootInterruptWait
	}
	if cancel == nil {
		// A request that already answered errBusy left the attempt
		// unwinding; a retry waits for it the same bounded way rather than
		// queuing behind it on opMu.
		if unwinding == nil {
			return false, nil
		}
		select {
		case <-unwinding:
			return false, nil
		case <-time.After(wait):
			return false, errBusy
		}
	}
	cancel()
	select {
	case <-done:
		return true, nil
	case <-time.After(wait):
		n.mu.Lock()
		n.bootUnwinding = done
		n.mu.Unlock()
		go func() {
			<-done
			n.mu.Lock()
			if n.bootUnwinding == done {
				n.bootUnwinding = nil
			}
			n.mu.Unlock()
			n.Boot()
		}()
		return false, errBusy
	}
}

// start brings the tailnet up and the relay on it. Caller holds opMu.
// Cancelling ctx abandons the attempt and leaves the state "starting".
func (n *Node) start(parent context.Context, authKey string, timeout time.Duration) error {
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
	ctx, cancel := context.WithTimeout(parent, timeout)
	st, err := srv.Up(ctx)
	cancel()
	if err != nil {
		srv.Close()
		if parent.Err() != nil {
			n.setState(stateStarting, "")
		} else {
			n.setState(stateError, err.Error())
		}
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
		HostUDP: func(local netip.Addr) (*net.UDPConn, error) {
			return net.ListenUDP("udp4", net.UDPAddrFromAddrPort(netip.AddrPortFrom(local, 0)))
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
	relay.ForgetForward = fwd.Forget
	n.mu.Lock()
	n.fwd = fwd
	n.mu.Unlock()
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
	// Timeouts as on the provisioning API, so a peer that opens connections
	// and stalls can't pile up goroutines in the radio's memory.
	telemetrySrv := &http.Server{
		Handler:           n.telemetryHandler(n.authorizer(lc)),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      10 * time.Second,
		IdleTimeout:       60 * time.Second,
		MaxHeaderBytes:    8 << 10,
	}
	closers = append(closers, telemetrySrv.Close)
	go telemetrySrv.Serve(tl)
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
	n.check = n.checker(lc)
	n.authorize = admitOnly(n.check)
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
	// Held from the snapshot to the assignment: discovery callbacks and
	// SetSharing refresh concurrently, and without this a refresh that read
	// "sharing on" could finish after one that withdrew the routes.
	n.routeMu.Lock()
	defer n.routeMu.Unlock()
	n.mu.Lock()
	lc, cfg, prev := n.lc, n.cfg, n.advertised
	n.mu.Unlock()
	if lc == nil && n.applyRoutes == nil {
		return
	}
	want := n.effectiveRoutes(cfg)
	if slices.Equal(want, prev) && prev != nil {
		return
	}
	apply := n.applyRoutes
	if apply == nil {
		apply = func(routes []netip.Prefix) error {
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			_, err := lc.EditPrefs(ctx, &ipn.MaskedPrefs{
				Prefs:              ipn.Prefs{AdvertiseRoutes: routes},
				AdvertiseRoutesSet: true,
			})
			return err
		}
	}
	if err := apply(want); err != nil {
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
	// Un-sharing takes effect at once, like the allowlist: end connections
	// to a device no longer offered.
	if ended := n.splices.unshare(n.shared); ended > 0 {
		log.Printf("ended %d connection(s) to devices no longer shared", ended)
	}
}

// SetSharing changes which LAN devices are shared, without leaving the
// tailnet. shareDiscovered nil leaves that setting unchanged.
func (n *Node) SetSharing(presented string, routes []string, shareDiscovered *bool) error {
	end, err := n.beginOp(presented, false)
	if err != nil {
		return err
	}
	n.mu.Lock()
	cfg := n.cfg
	n.mu.Unlock()
	cfg.Routes = routes
	if shareDiscovered != nil {
		cfg.ShareDiscovered = shareDiscovered
	}
	if err := n.save(cfg); err != nil {
		end()
		return err
	}
	n.mu.Lock()
	n.cfg = cfg
	n.mu.Unlock()
	end()
	n.RefreshRoutes()
	return nil
}

// beginOp admits an operator request and serializes it: the admin token is
// checked (unless firstUse allows an unprovisioned node), then any pending
// boot attempt is interrupted and opMu taken, then the token is checked
// again under opMu, where it can't be rotated underneath. Checking before
// the interrupt means a caller without the token can't keep restarting the
// boot; interrupting before taking opMu means a request never waits behind a
// tailnet login. end releases opMu and resumes the interrupted boot if the
// node still needs one (Boot does nothing for a node that is up or
// unprovisioned). Every operator request goes through here.
func (n *Node) beginOp(presented string, firstUse bool) (end func(), err error) {
	refused := func() bool {
		n.mu.Lock()
		needsToken := !firstUse || n.provisioned()
		n.mu.Unlock()
		return needsToken && !n.checkToken(presented)
	}
	if refused() {
		return nil, errUnauthorized
	}
	resume, err := n.interruptBoot()
	if err != nil {
		return nil, err
	}
	n.opMu.Lock()
	end = func() {
		n.opMu.Unlock()
		if resume {
			n.Boot()
		}
	}
	if refused() {
		end()
		return nil, errUnauthorized
	}
	return end, nil
}

// stop leaves the relay and tailnet. With logout it also deregisters the
// node and deletes its tailnet state, so the next start needs a new auth
// key; the error says what could not be undone. Caller holds opMu.
func (n *Node) stop(logout bool) error {
	n.mu.Lock()
	srv, relay, closers := n.srv, n.relay, n.closers
	n.srv, n.relay, n.closers, n.ip, n.dnsName, n.lc = nil, nil, nil, netip.Addr{}, "", nil
	n.authorize, n.check = nil, nil
	n.fwd = nil
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
	var errs []error
	if srv != nil {
		if logout {
			if lc, err := srv.LocalClient(); err == nil {
				ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
				if err := lc.Logout(ctx); err != nil {
					log.Printf("logout: %v", err)
					errs = append(errs, fmt.Errorf("the tailnet did not confirm the logout (remove the machine in the admin console): %w", err))
				}
				cancel()
			}
		}
		srv.Close()
	}
	if logout {
		if err := os.RemoveAll(n.tsnetDir()); err != nil {
			errs = append(errs, fmt.Errorf("could not delete the tailnet state: %w", err))
		}
	}
	return errors.Join(errs...)
}

// authorizer checks each tailnet caller with WhoIs against the current
// allowlist (logins or tags). An empty allowlist admits any tailnet peer.
// It fails closed: a caller whose identity can't be established is refused.
func (n *Node) authorizer(lc *local.Client) func(net.Addr) (string, bool) {
	return admitOnly(n.checker(lc))
}

// identityCheck is the allowlist verdict with its uncertainty kept: err is
// set when the caller's identity couldn't be established at all.
type identityCheck func(net.Addr) (who string, allowed bool, err error)

// admitOnly turns a check into an admission decision, refusing on error.
func admitOnly(check identityCheck) func(net.Addr) (string, bool) {
	return func(remote net.Addr) (string, bool) {
		who, ok, err := check(remote)
		if err != nil {
			return "unknown peer: " + err.Error(), false
		}
		return who, ok
	}
}

func (n *Node) checker(lc *local.Client) identityCheck {
	return func(remote net.Addr) (string, bool, error) {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		who, err := lc.WhoIs(ctx, remote.String())
		if err != nil {
			return "", false, err
		}
		id := who.Node.ComputedName
		if who.UserProfile != nil && !who.Node.IsTagged() {
			id = who.UserProfile.LoginName + " on " + id
		}
		n.mu.Lock()
		allow := append([]string{}, n.cfg.Allow...)
		n.mu.Unlock()
		if len(allow) == 0 {
			return id, true, nil
		}
		for _, a := range allow {
			if who.UserProfile != nil && !who.Node.IsTagged() && a == who.UserProfile.LoginName {
				return id, true, nil
			}
			for _, t := range who.Node.Tags {
				if a == t {
					return id, true, nil
				}
			}
		}
		return id, false, nil
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
//
// The token is checked here, under opMu, not by the caller: checked before
// the lock, two first-use requests could both see an unprovisioned node and
// the second would replace the first's node without its token.
func (n *Node) Provision(presented, authKey, hostname string, allow, routes []string, shareDiscovered *bool) (string, error) {
	// First use needs no token (beginOp).
	end, err := n.beginOp(presented, true)
	if err != nil {
		return "", err
	}
	defer end()

	token, hash := newToken()
	cfg := provisionConfig{TokenSHA256: hash, Hostname: hostname, Allow: allow, Routes: routes,
		ShareDiscovered: shareDiscovered}
	// Leave the previous tailnet first and fail closed if its state can't be
	// deleted: tsnet ignores a new auth key while a stored node exists, so
	// starting on top of it would keep the old identity while reporting a
	// new join. On that failure the saved settings and token are untouched,
	// so the caller's token still works for another try. (A logout the
	// tailnet didn't confirm is only logged: the state is gone either way.)
	if err := n.stop(true); err != nil {
		log.Printf("provision: leaving the previous tailnet: %v", err)
	}
	if _, err := os.Stat(n.tsnetDir()); !errors.Is(err, os.ErrNotExist) {
		n.setState(stateError, "could not delete the previous tailnet state")
		if err == nil {
			err = errors.New("it is still there")
		}
		return "", fmt.Errorf("could not delete the previous tailnet state, so the key was not used: %w", err)
	}
	// Then save, so a full or read-only state directory fails with the old
	// settings and token intact.
	if err := n.save(cfg); err != nil {
		n.setState(stateError, "could not save settings")
		return "", fmt.Errorf("could not save settings: %w", err)
	}
	n.mu.Lock()
	n.cfg = cfg
	n.mu.Unlock()

	start := n.startFn
	if start == nil {
		start = n.start
	}
	if err := start(context.Background(), authKey, 90*time.Second); err != nil {
		// A rejected key leaves the node unprovisioned rather than half-joined.
		if serr := n.stop(true); serr != nil {
			log.Printf("provision: cleaning up after a failed join: %v", serr)
		}
		n.mu.Lock()
		n.cfg = provisionConfig{}
		n.state = stateError
		n.mu.Unlock()
		if rerr := os.Remove(n.cfgPath()); rerr != nil && !errors.Is(rerr, os.ErrNotExist) {
			return "", errors.Join(err, fmt.Errorf("could not delete the saved settings, so they return after a restart: %w", rerr))
		}
		return "", err
	}
	return token, nil
}

// SetAllow changes who may connect, without leaving the tailnet, and ends
// every open connection the new list refuses (revokeDisallowed).
func (n *Node) SetAllow(presented string, allow []string) error {
	end, err := n.beginOp(presented, false)
	if err != nil {
		return err
	}
	n.mu.Lock()
	cfg := n.cfg
	cfg.Allow = allow
	n.mu.Unlock()
	if err := n.save(cfg); err != nil {
		end()
		return err
	}
	n.mu.Lock()
	n.cfg = cfg
	n.mu.Unlock()
	end()
	n.revokeDisallowed()
	return nil
}

// revokeDisallowed ends every open connection the allowlist no longer
// admits: relay sessions (the radio drops the client and unkeys anything it
// owned, Principle VI), side-channel transfers and station-device splices.
// The authorizer reads the list live, so this applies a just-saved change.
//
// Only a definite refusal ends a connection. Admission fails closed, but a
// WhoIs that couldn't answer (a tailscaled hiccup, a timeout) is no
// evidence that someone still on the list should be cut off mid-contact.
func (n *Node) revokeDisallowed() {
	n.mu.Lock()
	authorize, check, relay, fwd := n.authorize, n.check, n.relay, n.fwd
	n.mu.Unlock()
	if authorize == nil {
		return
	}
	refused := authorize
	if check != nil {
		refused = func(a net.Addr) (string, bool) {
			who, ok, err := check(a)
			if err != nil {
				log.Printf("allowlist changed: keeping %s, its identity is unavailable: %v", a, err)
				return who, true
			}
			return who, ok
		}
	}
	ended := n.splices.revoke(refused)
	if relay != nil {
		ended += relay.Revoke(refused)
	}
	if fwd != nil {
		ended += fwd.splices.revoke(refused)
	}
	if ended > 0 {
		log.Printf("allowlist changed: ended %d connection(s) it no longer admits", ended)
	}
}

// SignOut leaves the tailnet, deregisters the node and forgets the admin
// token, returning the shim to its unprovisioned state.
//
// The saved settings are deleted first: if that fails nothing has changed,
// the node stays provisioned and the caller's token stays valid, so the
// client keeps it. Once they are gone a restart can't revive the old token,
// and any later failure (logout, tailnet state) is returned but leaves the
// node signed out.
func (n *Node) SignOut(presented string) error {
	end, err := n.beginOp(presented, false)
	if err != nil {
		return err
	}
	defer end()
	if err := os.Remove(n.cfgPath()); err != nil && !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("could not delete the saved settings, so nothing was changed: %w", err)
	}
	err = n.stop(true)
	n.mu.Lock()
	n.cfg = provisionConfig{}
	n.state, n.lastError = stateUnprovisioned, ""
	n.mu.Unlock()
	if err != nil {
		return fmt.Errorf("signed out, but %w", err)
	}
	return nil
}

// errUnauthorized: the presented admin token doesn't match (or a first-use
// provision lost the race to another).
var errUnauthorized = errors.New("admin token required")

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

// shared reports whether a is inside a route the node advertises now.
func (n *Node) shared(a netip.Addr) bool {
	n.mu.Lock()
	defer n.mu.Unlock()
	for _, p := range n.advertised {
		if p.Contains(a) {
			return true
		}
	}
	return false
}

// isRadio reports whether a is the radio: RadioAddr, or any address of the
// host the container shares with it.
func (n *Node) isRadio(a netip.Addr) bool {
	a = a.Unmap()
	if ra, err := netip.ParseAddr(n.RadioAddr); err == nil && ra.Unmap() == a {
		return true
	}
	n.mu.Lock()
	list := n.localAddrs
	n.mu.Unlock()
	return n.locals.has(a, list)
}

// subnetTCP handles a TCP flow addressed to one of this node's advertised
// subnet routes (a station device): check the caller against the allowlist,
// then dial the device from the radio's network and splice. The tailnet's
// ACLs admitting the sender is not enough: the allowlist governs station
// devices exactly as it governs the radio. A route wide enough to contain
// the radio's own address never reaches the radio itself: its API would be
// spliced with no relay in front, and its provisioning API would see a LAN
// caller. Flows to anything else are left alone.
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
		authorize := n.authorize
		n.mu.Unlock()
		if authorize == nil {
			log.Printf("subnet %s -> %s: refused, the node is not running", src, dst)
			return
		}
		if who, ok := authorize(net.TCPAddrFromAddrPort(src)); !ok {
			log.Printf("subnet %s -> %s: refused %s", src, dst, who)
			return
		}
		if n.isRadio(dst.Addr()) {
			log.Printf("subnet %s -> %s: refused, that is the radio itself", src, dst)
			return
		}
		// Only an admitted flow counts as evidence that the route is in use.
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
		defer n.splices.add(net.TCPAddrFromAddrPort(src), dst.Addr(), c, r)()
		// Check again now that the splice is registered, so an allowlist
		// saved during the dial above can't miss it (see Relay.handle); and
		// the route must still be shared (RefreshRoutes closes splices to
		// routes it withdraws, and this one may have registered just after).
		if who, ok := authorize(net.TCPAddrFromAddrPort(src)); !ok {
			log.Printf("subnet %s -> %s: %s is no longer on the allowlist", src, dst, who)
			return
		}
		if !n.shared(dst.Addr()) {
			log.Printf("subnet %s -> %s: no longer shared", src, dst)
			return
		}
		log.Printf("subnet %s -> %s: connected", src, dst)
		done := make(chan struct{}, 2)
		go func() { io.Copy(r, c); closeWrite(r); done <- struct{}{} }()
		go func() { io.Copy(c, r); closeWrite(c); done <- struct{}{} }()
		<-done
		<-done
	}, true
}
