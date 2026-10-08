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
	"log"
	"net"
	"net/netip"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"tailscale.com/client/local"
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
}

// Node owns the tailnet membership and the relay that runs on it. Keys are
// swapped in-process: nothing is known to restart a container whose process
// exits, so the shim never relies on exiting to reconfigure itself.
type Node struct {
	StateDir  string
	RadioAddr string
	MaxMTU    int

	opMu sync.Mutex // serializes provision / sign-out / start

	mu        sync.Mutex
	cfg       provisionConfig
	state     string
	lastError string
	srv       *tsnet.Server
	relay     *Relay
	closers   []func() error
	ip        netip.Addr
	dnsName   string
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

	n.mu.Lock()
	n.srv, n.relay, n.closers, n.ip = srv, relay, closers, ip4
	n.dnsName = strings.TrimSuffix(st.Self.DNSName, ".")
	n.state, n.lastError = stateRunning, ""
	n.mu.Unlock()
	log.Printf("tailnet node %q up at %s (%s)", cfg.Hostname, ip4, n.dnsName)
	go reportHealth(lc)
	return nil
}

// stop leaves the relay and tailnet. With logout it also deregisters the
// node, so the next start needs a new auth key. Caller holds opMu.
func (n *Node) stop(logout bool) {
	n.mu.Lock()
	srv, relay, closers := n.srv, n.relay, n.closers
	n.srv, n.relay, n.closers, n.ip, n.dnsName = nil, nil, nil, netip.Addr{}, ""
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
func (n *Node) Provision(authKey, hostname string, allow []string) (string, error) {
	n.opMu.Lock()
	defer n.opMu.Unlock()

	n.stop(true)
	token, hash := newToken()
	cfg := provisionConfig{TokenSHA256: hash, Hostname: hostname, Allow: allow}
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
	LastError   string   `json:"last_error"`
	// Command verbs relayed so far (never arguments), for diagnosing what a
	// client such as a Maestro asks the radio for.
	RecentCommands []string `json:"recent_commands"`
}

func (n *Node) Status() Status {
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
	if n.relay != nil {
		st.Sessions = n.relay.SessionCount()
		st.RecentCommands = n.relay.RecentCommands()
	}
	return st
}
