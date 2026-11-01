package main

import (
	"context"
	"errors"
	"fmt"
	"log"
	"os"
	"regexp"
	"time"

	"tailscale.com/client/local"
	"tailscale.com/ipn/ipnstate"
)

// Sign-in by QR code. Joining without an auth key, tsnet asks the control
// plane for an interactive login and publishes its URL in the node's own
// status; AetherSDR shows that URL as a QR code to scan and approve on a
// phone, as AetherConsole's network setup does. Whoever opens the URL first
// claims the radio for their tailnet, so it goes only to the caller that
// started the join or holds the admin token: never into /v1/status, never
// into a log.

const (
	stateNeedsLogin    = "needs_login"    // waiting for someone to open the sign-in URL
	stateNeedsApproval = "needs_approval" // signed in; a tailnet admin must approve the device
)

var urlInLog = regexp.MustCompile(`[a-zA-Z][a-zA-Z0-9+.-]*://\S+`)

// withoutURLs wraps a logger so no URL reaches it. tsnet repeats the sign-in
// URL to its user log every few seconds while it waits, and the shim's log
// goes to the radio's journal and, in development, to SHIM_LOG_URL.
func withoutURLs(logf func(string, ...any)) func(string, ...any) {
	return func(format string, args ...any) {
		logf("%s", urlInLog.ReplaceAllString(fmt.Sprintf(format, args...), "<url withheld>"))
	}
}

// loginInfo is the tailnet backend's view of the node's sign-in.
type loginInfo struct {
	Backend   string     // ipn state: NeedsLogin, NeedsMachineAuth, Running, …
	AuthURL   string     // set while NeedsLogin, once the control plane sent it
	KeyExpiry *time.Time // nil: the node key doesn't expire
}

// loginReader reads loginInfo; a seam, so tests need no tailnet.
type loginReader func(context.Context) (loginInfo, error)

func lcLoginReader(lc *local.Client) loginReader {
	return func(ctx context.Context) (loginInfo, error) {
		st, err := lc.StatusWithoutPeers(ctx)
		if err != nil {
			return loginInfo{}, err
		}
		return loginInfoOf(st), nil
	}
}

func loginInfoOf(st *ipnstate.Status) loginInfo {
	li := loginInfo{Backend: st.BackendState, AuthURL: st.AuthURL}
	if st.Self != nil {
		li.KeyExpiry = st.Self.KeyExpiry
	}
	return li
}

// pendingState is the shim state a backend waiting on a person maps to, or
// "" when it isn't waiting on one. NeedsLogin counts only once its URL has
// arrived: until then the backend is still talking to the control plane.
func pendingState(li loginInfo) string {
	switch {
	case li.Backend == "NeedsLogin" && li.AuthURL != "":
		return stateNeedsLogin
	case li.Backend == "NeedsMachineAuth":
		return stateNeedsApproval
	}
	return ""
}

// setLoginState moves between the joining states without overriding a
// sign-out or an error that happened meanwhile.
func (n *Node) setLoginState(state string) {
	n.mu.Lock()
	defer n.mu.Unlock()
	switch n.state {
	case stateStarting, stateRunning, stateNeedsLogin, stateNeedsApproval:
		n.state, n.lastError = state, ""
	}
}

// loginPoll is how often a joining node checks whether it is waiting on a
// person; a variable for tests.
var loginPoll = time.Second

// watchJoin runs while start() waits for the tailnet. Once the node waits on
// a person (a sign-in or an admin's approval) it says so in the state, and an
// interactive join calls disarm: the attempt then waits for them instead of
// timing out, which would mint a new URL under the QR code being scanned. A
// join with an auth key keeps its timeout, so a refused key still fails.
func (n *Node) watchJoin(ctx context.Context, read loginReader, interactive bool, disarm func()) {
	t := time.NewTicker(loginPoll)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		}
		rctx, cancel := context.WithTimeout(ctx, 3*time.Second)
		li, err := read(rctx)
		cancel()
		if err != nil {
			continue
		}
		// A keyed join that the control plane answers with a sign-in URL
		// has had its key refused: it stays "starting" until its timeout.
		if s := pendingState(li); s != "" && interactive {
			n.setLoginState(s)
			disarm()
		}
	}
}

// signInKeepPoll is how often a running node checks its sign-in.
var signInKeepPoll = 15 * time.Second

// keepSignedIn runs while the node is up. A node key expires (180 days by
// default for a node signed in by URL), and then the node needs a new
// sign-in: it shows that, asks the control plane for a URL the admin-token
// holder can fetch, and returns to running once someone signs in. It also
// tracks the key's expiry for /v1/status.
func (n *Node) keepSignedIn(ctx context.Context, read loginReader, startLogin func(context.Context) error) {
	asked := false
	check := func() {
		rctx, cancel := context.WithTimeout(ctx, 5*time.Second)
		li, err := read(rctx)
		cancel()
		if err != nil {
			return
		}
		n.mu.Lock()
		n.keyExpiry = li.KeyExpiry
		n.mu.Unlock()
		switch {
		case li.Backend == "Running":
			asked = false
			n.setLoginState(stateRunning)
		case li.Backend == "NeedsLogin":
			if li.AuthURL == "" && !asked && startLogin != nil {
				asked = true
				lctx, cancel := context.WithTimeout(ctx, 10*time.Second)
				if err := startLogin(lctx); err != nil {
					asked = false
					log.Printf("sign-in expired; asking for a new sign-in failed: %v", err)
				}
				cancel()
			}
			if s := pendingState(li); s != "" {
				n.setLoginState(s)
			}
		case li.Backend == "NeedsMachineAuth":
			n.setLoginState(stateNeedsApproval)
		}
	}
	check()
	t := time.NewTicker(signInKeepPoll)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			check()
		}
	}
}

// LoginReply is what GET /v1/login returns to the admin-token holder.
type LoginReply struct {
	State     string `json:"state"`
	AuthURL   string `json:"auth_url,omitempty"`
	KeyExpiry string `json:"key_expiry,omitempty"`
}

// Login reports the node's sign-in, with the URL while one is wanted. It
// checks the token without beginOp: an operator request interrupts the boot
// attempt, and that attempt is the tailnet instance holding the URL.
func (n *Node) Login(presented string) (LoginReply, error) {
	if !n.checkToken(presented) {
		return LoginReply{}, errUnauthorized
	}
	n.mu.Lock()
	read, state := n.loginRead, n.state
	n.mu.Unlock()
	reply := LoginReply{State: state}
	if read == nil {
		return reply, nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	li, err := read(ctx)
	if err != nil {
		return reply, nil
	}
	if li.Backend == "NeedsLogin" {
		reply.AuthURL = li.AuthURL
	}
	if li.KeyExpiry != nil {
		reply.KeyExpiry = li.KeyExpiry.UTC().Format(time.RFC3339)
	}
	return reply, nil
}

// loginURLWait bounds how long an interactive provision waits for the
// control plane's URL, inside AetherSDR's request timeout.
const loginURLWait = 30 * time.Second

// ProvisionInteractive joins the tailnet without an auth key: it saves the
// settings and a new admin token, starts the join in the background, and
// returns the token with the sign-in URL once the control plane sends it.
// The token comes back even without a URL (a slow control plane), so the
// caller can fetch the URL from /v1/login; a token-less provisioned node
// could never be changed again.
func (n *Node) ProvisionInteractive(presented, hostname string, allow, routes []string, shareDiscovered *bool) (token, authURL string, err error) {
	end, err := n.beginOp(presented, true)
	if err != nil {
		return "", "", err
	}
	token, hash := newToken()
	cfg := provisionConfig{TokenSHA256: hash, Hostname: hostname, Allow: allow, Routes: routes,
		ShareDiscovered: shareDiscovered}
	if err := n.replaceNode(cfg); err != nil {
		end()
		return "", "", err
	}
	n.setState(stateStarting, "")
	end()
	n.Boot()
	log.Printf("provision: waiting for a sign-in (hostname %q)", hostname)

	deadline := time.Now().Add(loginURLWait)
	for time.Now().Before(deadline) {
		r, lerr := n.Login(token)
		if lerr != nil {
			// Signed out or re-provisioned meanwhile: this token is dead.
			return "", "", lerr
		}
		if r.AuthURL != "" {
			// The backend has the URL before watchJoin's next tick says so;
			// say it now, so the reply's status agrees with its URL.
			n.setLoginState(stateNeedsLogin)
			return token, r.AuthURL, nil
		}
		if r.State == stateRunning || r.State == stateNeedsApproval {
			return token, "", nil
		}
		if r.State == stateError {
			n.mu.Lock()
			msg := n.lastError
			n.mu.Unlock()
			return token, "", fmt.Errorf("the tailnet sign-in didn't start: %s", msg)
		}
		time.Sleep(250 * time.Millisecond)
	}
	return token, "", nil
}

// replaceNode leaves the previous tailnet and saves cfg as the node's
// settings, failing closed: tsnet ignores a new key or login while a stored
// node exists, so starting on top of it would keep the old identity while
// reporting a new join. On failure the saved settings and token are
// untouched, so the caller's token still works for another try. (A logout
// the tailnet didn't confirm is only logged: the state is gone either way.)
// Caller holds opMu.
func (n *Node) replaceNode(cfg provisionConfig) error {
	if err := n.stop(true); err != nil {
		log.Printf("provision: leaving the previous tailnet: %v", err)
	}
	if _, err := os.Stat(n.tsnetDir()); !errors.Is(err, os.ErrNotExist) {
		n.setState(stateError, "could not delete the previous tailnet state")
		if err == nil {
			err = errors.New("it is still there")
		}
		return fmt.Errorf("could not delete the previous tailnet state, so the key was not used: %w", err)
	}
	// Then save, so a full or read-only state directory fails with the old
	// settings and token intact.
	if err := n.save(cfg); err != nil {
		n.setState(stateError, "could not save settings")
		return fmt.Errorf("could not save settings: %w", err)
	}
	n.mu.Lock()
	n.cfg = cfg
	n.mu.Unlock()
	return nil
}
