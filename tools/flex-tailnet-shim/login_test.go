package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

const testAuthURL = "https://login.tailscale.com/a/0123456789abcdef"

// fakeBackend is a loginReader whose answer the test sets.
type fakeBackend struct {
	mu sync.Mutex
	li loginInfo
}

func (f *fakeBackend) set(li loginInfo) {
	f.mu.Lock()
	f.li = li
	f.mu.Unlock()
}

func (f *fakeBackend) read(context.Context) (loginInfo, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.li, nil
}

func fastLoginPolls(t *testing.T) {
	t.Helper()
	oldJoin, oldKeep := loginPoll, signInKeepPoll
	loginPoll, signInKeepPoll = 5*time.Millisecond, 5*time.Millisecond
	t.Cleanup(func() { loginPoll, signInKeepPoll = oldJoin, oldKeep })
}

func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(2 * time.Millisecond)
	}
}

func (n *Node) currentState() string {
	n.mu.Lock()
	defer n.mu.Unlock()
	return n.state
}

func TestNeedsLoginCountsOnlyOnceTheURLArrives(t *testing.T) {
	for _, c := range []struct {
		li   loginInfo
		want string
	}{
		{loginInfo{Backend: "NeedsLogin"}, ""},
		{loginInfo{Backend: "NeedsLogin", AuthURL: testAuthURL}, stateNeedsLogin},
		{loginInfo{Backend: "NeedsMachineAuth"}, stateNeedsApproval},
		{loginInfo{Backend: "Running"}, ""},
		{loginInfo{Backend: "Starting"}, ""},
	} {
		if got := pendingState(c.li); got != c.want {
			t.Errorf("%+v: got %q, want %q", c.li, got, c.want)
		}
	}
}

// Without an auth key, the join stops its timeout once it waits on a person:
// timing out would start over with a new URL under the QR code being scanned.
func TestInteractiveJoinWaitsForTheSignIn(t *testing.T) {
	fastLoginPolls(t)
	n := newTestNode(t, "tok")
	n.setState(stateStarting, "")
	fb := &fakeBackend{}
	fb.set(loginInfo{Backend: "NeedsLogin"})
	var disarmed atomic.Bool
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go n.watchJoin(ctx, fb.read, true, func() { disarmed.Store(true) })

	time.Sleep(30 * time.Millisecond)
	if disarmed.Load() || n.currentState() != stateStarting {
		t.Fatal("disarmed before the sign-in URL arrived")
	}
	fb.set(loginInfo{Backend: "NeedsLogin", AuthURL: testAuthURL})
	waitFor(t, "the timeout to be disarmed", disarmed.Load)
	if s := n.currentState(); s != stateNeedsLogin {
		t.Fatalf("state %q, want %q", s, stateNeedsLogin)
	}
}

// With an auth key the timeout stays armed, so a refused key still fails and
// unprovisions instead of waiting forever.
func TestKeyedJoinKeepsItsTimeout(t *testing.T) {
	fastLoginPolls(t)
	n := newTestNode(t, "tok")
	n.setState(stateStarting, "")
	fb := &fakeBackend{}
	fb.set(loginInfo{Backend: "NeedsLogin", AuthURL: testAuthURL})
	var disarmed atomic.Bool
	ctx, cancel := context.WithCancel(context.Background())
	go n.watchJoin(ctx, fb.read, false, func() { disarmed.Store(true) })
	time.Sleep(50 * time.Millisecond)
	cancel()
	if disarmed.Load() || n.currentState() != stateStarting {
		t.Fatalf("a keyed join disarmed its timeout or left starting (%s)", n.currentState())
	}
}

// An expired sign-in asks the control plane for one new URL, shows that the
// node needs a sign-in, and returns to running once someone signs in.
func TestExpiredSignInAsksOnceAndRecovers(t *testing.T) {
	fastLoginPolls(t)
	n := newTestNode(t, "tok")
	n.setState(stateRunning, "")
	expiry := time.Date(2027, 4, 30, 12, 0, 0, 0, time.UTC)
	fb := &fakeBackend{}
	fb.set(loginInfo{Backend: "Running", KeyExpiry: &expiry})
	var asks atomic.Int32
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go n.keepSignedIn(ctx, fb.read, func(context.Context) error { asks.Add(1); return nil })

	waitFor(t, "the expiry to be recorded", func() bool { return n.Status().KeyExpiry == "2027-04-30T12:00:00Z" })
	fb.set(loginInfo{Backend: "NeedsLogin"})
	waitFor(t, "a new sign-in to be asked for", func() bool { return asks.Load() == 1 })
	time.Sleep(30 * time.Millisecond)
	if asks.Load() != 1 {
		t.Fatalf("asked %d times while waiting for the URL", asks.Load())
	}
	fb.set(loginInfo{Backend: "NeedsLogin", AuthURL: testAuthURL})
	waitFor(t, "needs_login", func() bool { return n.currentState() == stateNeedsLogin })
	fb.set(loginInfo{Backend: "Running"})
	waitFor(t, "running again", func() bool { return n.currentState() == stateRunning })
	if n.Status().KeyExpiry != "" {
		t.Fatal("a key that no longer expires still shows an expiry")
	}
}

// A sign-out that happened meanwhile is never overwritten by the watcher.
func TestLoginStateNeverRevivesASignedOutNode(t *testing.T) {
	n := newTestNode(t, "")
	n.setLoginState(stateNeedsLogin)
	if s := n.currentState(); s != stateUnprovisioned {
		t.Fatalf("state %q", s)
	}
}

// The URL claims the radio for whoever opens it: only the token holder
// gets it, and never through /v1/status.
func TestSignInURLGoesOnlyToTheTokenHolder(t *testing.T) {
	n := newTestNode(t, "tok")
	n.setState(stateNeedsLogin, "")
	fb := &fakeBackend{}
	fb.set(loginInfo{Backend: "NeedsLogin", AuthURL: testAuthURL})
	n.loginRead = fb.read
	h := NewAPI(n)

	if rec := call(t, h, "GET", "/v1/login", "192.168.1.10:5000", "", ""); rec.Code != http.StatusUnauthorized {
		t.Fatalf("no token: %d", rec.Code)
	}
	if rec := call(t, h, "GET", "/v1/login", "192.168.1.10:5000", "wrong", ""); rec.Code != http.StatusUnauthorized {
		t.Fatalf("wrong token: %d", rec.Code)
	}
	rec := call(t, h, "GET", "/v1/login", "192.168.1.10:5000", "tok", "")
	var lr LoginReply
	if err := json.Unmarshal(rec.Body.Bytes(), &lr); err != nil || lr.AuthURL != testAuthURL || lr.State != stateNeedsLogin {
		t.Fatalf("token holder got %d %s", rec.Code, rec.Body)
	}
	if rec := call(t, h, "GET", "/v1/status", "192.168.1.10:5000", "", ""); strings.Contains(rec.Body.String(), "login.tailscale.com") {
		t.Fatalf("status leaks the URL: %s", rec.Body)
	}
	// Once signed in there is no URL to hand out, even if the backend still
	// remembers the one that was used.
	fb.set(loginInfo{Backend: "Running", AuthURL: testAuthURL})
	rec = call(t, h, "GET", "/v1/login", "192.168.1.10:5000", "tok", "")
	if strings.Contains(rec.Body.String(), "auth_url") {
		t.Fatalf("a running node returned a URL: %s", rec.Body)
	}
}

// A provision without a key returns the new token and the URL, and leaves
// the join running in the background for the person to finish.
func TestProvisionWithoutAKeyReturnsTheSignInURL(t *testing.T) {
	n := newTestNode(t, "")
	fb := &fakeBackend{}
	fb.set(loginInfo{Backend: "NeedsLogin", AuthURL: testAuthURL})
	var gotKey atomic.Value
	n.startFn = func(ctx context.Context, authKey string, _ time.Duration) error {
		gotKey.Store(authKey)
		// The backend has the URL; watchJoin hasn't ticked yet, so the
		// node still says "starting" until the provision reply sets it.
		n.mu.Lock()
		n.loginRead = fb.read
		n.mu.Unlock()
		<-ctx.Done() // waits for the sign-in, as the real join does
		return ctx.Err()
	}
	h := NewAPI(n)
	rec := call(t, h, "POST", "/v1/provision", "192.168.1.10:5000", "",
		`{"auth_key":"","hostname":"flex-qr","allow":[],"routes":[]}`)
	var reply struct {
		AdminToken string `json:"admin_token"`
		AuthURL    string `json:"auth_url"`
		Status     Status `json:"status"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &reply); err != nil || rec.Code != http.StatusOK {
		t.Fatalf("%d %s", rec.Code, rec.Body)
	}
	if reply.AdminToken == "" || reply.AuthURL != testAuthURL || !reply.Status.Provisioned ||
		reply.Status.State != stateNeedsLogin {
		t.Fatalf("reply %+v", reply)
	}
	if k, _ := gotKey.Load().(string); k != "" {
		t.Fatalf("the join was given key %q", k)
	}
	if !n.checkToken(reply.AdminToken) {
		t.Fatal("the returned token isn't the node's")
	}
	// Stop the background join this test left waiting for its sign-in.
	if _, err := n.interruptBoot(); err != nil {
		t.Fatal(err)
	}
}

// A sign-in by URL that never completed belongs to no tailnet, so it can be
// started over without the token (the window that began it may be gone);
// once the node has joined, the token is required again.
func TestANeverCompletedSignInCanBeStartedOverWithoutTheToken(t *testing.T) {
	n := newTestNode(t, "")
	fb := &fakeBackend{}
	fb.set(loginInfo{Backend: "NeedsLogin", AuthURL: testAuthURL})
	n.startFn = func(ctx context.Context, _ string, _ time.Duration) error {
		n.mu.Lock()
		n.loginRead = fb.read
		n.mu.Unlock()
		<-ctx.Done()
		return ctx.Err()
	}
	first, _, err := n.ProvisionInteractive("", "flex-qr", nil, nil, nil)
	if err != nil || first == "" {
		t.Fatalf("first: %q %v", first, err)
	}
	if !n.Status().Claimable {
		t.Fatal("a pending sign-in isn't claimable")
	}
	second, url, err := n.ProvisionInteractive("", "flex-qr", nil, nil, nil)
	if err != nil || second == "" || second == first || url != testAuthURL {
		t.Fatalf("token-less restart: %q %q %v", second, url, err)
	}
	if _, err := n.interruptBoot(); err != nil {
		t.Fatal(err)
	}
	// The node joins: the window closes, and survives a restart of the shim.
	n.markJoined()
	if n.Status().Claimable {
		t.Fatal("a joined node is still claimable")
	}
	if _, _, err := n.ProvisionInteractive("", "flex-qr", nil, nil, nil); !errors.Is(err, errUnauthorized) {
		t.Fatalf("a joined node re-provisioned without its token: %v", err)
	}
	reloaded := &Node{StateDir: n.StateDir}
	if err := reloaded.load(); err != nil || reloaded.cfg.PendingSignIn {
		t.Fatalf("joined state not saved: %v %+v", err, reloaded.cfg)
	}
}

// A keyed provision never opens the token-free window.
func TestAKeyedNodeIsNeverClaimable(t *testing.T) {
	n := newTestNode(t, "")
	n.startFn = inertStart(n)
	if _, err := n.Provision("", "tskey-auth-k", "flex-test", nil, nil, nil); err != nil {
		t.Fatal(err)
	}
	if n.Status().Claimable {
		t.Fatal("a keyed node is claimable")
	}
	if _, _, err := n.ProvisionInteractive("", "flex-qr", nil, nil, nil); !errors.Is(err, errUnauthorized) {
		t.Fatalf("taken over without the token: %v", err)
	}
}

// A malformed key is still refused; only an absent one means sign-in by URL.
func TestProvisionStillRefusesAMalformedKey(t *testing.T) {
	h := NewAPI(newTestNode(t, ""))
	rec := call(t, h, "POST", "/v1/provision", "192.168.1.10:5000", "", `{"auth_key":"nope","hostname":"x"}`)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("got %d", rec.Code)
	}
}

// tsnet logs the sign-in URL every few seconds while it waits; the shim's
// log must never carry it.
func TestUserLogWithholdsURLs(t *testing.T) {
	var got []string
	logf := withoutURLs(func(format string, args ...any) { got = append(got, fmt.Sprintf(format, args...)) })
	logf("To start this tsnet server, restart with TS_AUTHKEY set, or go to: %s", testAuthURL)
	logf("self-hosted: %s and %s", "https://headscale.example/register/abc", "plain text 100%")
	if strings.Contains(strings.Join(got, "\n"), "://") || !strings.Contains(got[0], "<url withheld>") || !strings.Contains(got[1], "100%") {
		t.Fatalf("logged %q", got)
	}
}
