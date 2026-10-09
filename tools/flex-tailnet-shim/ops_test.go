package main

import (
	"bufio"
	"context"
	"errors"
	"io"
	"net"
	"net/netip"
	"os"
	"strings"
	"sync"
	"testing"
	"time"
)

// Socket-free regressions for the Node's serialized operations: no tsnet,
// no radio. startFn and applyRoutes stand in for the tailnet.

func inertStart(n *Node) func(context.Context, string, time.Duration) error {
	return func(context.Context, string, time.Duration) error {
		n.setState(stateRunning, "")
		return nil
	}
}

// Two first-use provisions race: only the first may succeed without a token.
func TestConcurrentFirstUseProvisionNeedsTheFirstToken(t *testing.T) {
	n := newTestNode(t, "")
	release := make(chan struct{})
	entered := make(chan struct{}, 2)
	n.startFn = func(context.Context, string, time.Duration) error {
		entered <- struct{}{}
		<-release
		n.setState(stateRunning, "")
		return nil
	}

	var wg sync.WaitGroup
	var firstToken string
	var firstErr, secondErr error
	wg.Add(1)
	go func() {
		defer wg.Done()
		firstToken, firstErr = n.Provision("", "tskey-auth-first", "client-1", nil, nil, nil)
	}()
	<-entered // the first holds opMu inside start
	wg.Add(1)
	go func() {
		defer wg.Done()
		_, secondErr = n.Provision("", "tskey-auth-second", "client-2", nil, nil, nil)
	}()
	time.Sleep(50 * time.Millisecond) // let the second queue behind opMu
	close(release)
	wg.Wait()

	if firstErr != nil || firstToken == "" {
		t.Fatalf("first provision: token %q, err %v", firstToken, firstErr)
	}
	if !errors.Is(secondErr, errUnauthorized) {
		t.Fatalf("queued no-token provision: err %v, want errUnauthorized", secondErr)
	}
	if st := n.Status(); st.Hostname != "client-1" || !n.checkToken(firstToken) {
		t.Fatalf("the second request replaced the first's node: %+v", st)
	}
}

// A token rotated by a re-provision no longer authorizes anything.
func TestRotatedTokenIsRefused(t *testing.T) {
	n := newTestNode(t, "")
	n.startFn = inertStart(n)
	old, err := n.Provision("", "tskey-auth-a", "flex-test", nil, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	cur, err := n.Provision(old, "tskey-auth-b", "flex-test", nil, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := n.SetAllow(old, []string{"tag:ops"}); !errors.Is(err, errUnauthorized) {
		t.Fatalf("SetAllow with the rotated token: %v", err)
	}
	if err := n.SetSharing(old, nil, nil); !errors.Is(err, errUnauthorized) {
		t.Fatalf("SetSharing with the rotated token: %v", err)
	}
	if err := n.SignOut(old); !errors.Is(err, errUnauthorized) {
		t.Fatalf("SignOut with the rotated token: %v", err)
	}
	if err := n.SetAllow(cur, []string{"tag:ops"}); err != nil {
		t.Fatalf("SetAllow with the current token: %v", err)
	}
}

// A sign-out that cannot delete the saved settings changes nothing, so the
// client's token stays valid, and a restart does not revive anything else.
func TestSignOutThatCannotDeleteSettingsKeepsTheToken(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("root ignores directory permissions")
	}
	const token = "test-admin-token"
	n := newTestNode(t, token)
	if err := os.Chmod(n.StateDir, 0o500); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.Chmod(n.StateDir, 0o700) })

	if err := n.SignOut(token); err == nil {
		t.Fatal("sign-out reported success with an undeletable provision.json")
	}
	if !n.Status().Provisioned || !n.checkToken(token) {
		t.Fatal("a failed sign-out dropped the live token")
	}
	reloaded := &Node{StateDir: n.StateDir}
	if err := reloaded.load(); err != nil {
		t.Fatal(err)
	}
	if !reloaded.provisioned() || !reloaded.checkToken(token) {
		t.Fatal("after a restart the token no longer matches what the client kept")
	}

	os.Chmod(n.StateDir, 0o700)
	if err := n.SignOut(token); err != nil {
		t.Fatalf("sign-out: %v", err)
	}
	reloaded = &Node{StateDir: n.StateDir}
	if err := reloaded.load(); err != nil {
		t.Fatal(err)
	}
	if reloaded.provisioned() {
		t.Fatal("a completed sign-out came back after a restart")
	}
}

// A refresh that read "sharing on" and stalls must not re-advertise after
// the operator withdrew sharing.
func TestStaleRouteRefreshCannotUndoAWithdrawal(t *testing.T) {
	const token = "test-admin-token"
	n := newTestNode(t, token)
	n.cfg.Routes = []string{"192.168.50.10"}
	if err := n.save(n.cfg); err != nil {
		t.Fatal(err)
	}

	var mu sync.Mutex
	var applied [][]netip.Prefix
	stall := make(chan struct{})
	stalled := make(chan struct{})
	first := true
	n.applyRoutes = func(routes []netip.Prefix) error {
		mu.Lock()
		isFirst := first
		first = false
		mu.Unlock()
		if isFirst {
			close(stalled)
			<-stall
		}
		mu.Lock()
		applied = append(applied, routes)
		mu.Unlock()
		return nil
	}

	var wg sync.WaitGroup
	wg.Add(1)
	go func() { defer wg.Done(); n.RefreshRoutes() }() // e.g. a discovery callback
	<-stalled
	off := false
	wg.Add(1)
	go func() {
		defer wg.Done()
		if err := n.SetSharing(token, nil, &off); err != nil {
			t.Error(err)
		}
	}()
	time.Sleep(50 * time.Millisecond)
	close(stall)
	wg.Wait()

	mu.Lock()
	defer mu.Unlock()
	if len(applied) == 0 || len(applied[len(applied)-1]) != 0 {
		t.Fatalf("last routes applied %v, want none", applied)
	}
	n.mu.Lock()
	defer n.mu.Unlock()
	if len(n.advertised) != 0 {
		t.Fatalf("advertised %v after sharing was withdrawn", n.advertised)
	}
}

func TestReadLineIsBounded(t *testing.T) {
	br := bufio.NewReaderSize(strings.NewReader("C1|client gui\nshort\n"+strings.Repeat("x", 300)+"\ntail"), 64)
	for _, want := range []string{"C1|client gui\n", "short\n"} {
		got, err := readLine(br, 128)
		if err != nil || string(got) != want {
			t.Fatalf("readLine = %q, %v; want %q", got, err, want)
		}
	}
	if _, err := readLine(br, 128); !errors.Is(err, errLineTooLong) {
		t.Fatalf("a 301-byte line under a 128-byte cap: %v", err)
	}

	br = bufio.NewReaderSize(strings.NewReader("no newline"), 64)
	if got, err := readLine(br, 128); string(got) != "no newline" || err != io.EOF {
		t.Fatalf("final unterminated line = %q, %v", got, err)
	}
}

// Re-keying must not start on top of tailnet state it couldn't delete: tsnet
// would keep the old node and ignore the new key. The old token stays valid.
func TestProvisionFailsClosedWhenOldStateRemains(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("root ignores directory permissions")
	}
	const token = "test-admin-token"
	n := newTestNode(t, token)
	started := false
	n.startFn = func(context.Context, string, time.Duration) error { started = true; return nil }
	state := n.tsnetDir()
	if err := os.MkdirAll(state, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(state+"/tailscaled.state", []byte("old node"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(state, 0o500); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.Chmod(state, 0o700) })

	newToken, err := n.Provision(token, "tskey-auth-new", "flex-test", nil, nil, nil)
	if err == nil || newToken != "" || started {
		t.Fatalf("provision over undeletable state: token %q, err %v, started %v", newToken, err, started)
	}
	if !n.checkToken(token) {
		t.Fatal("the failed re-key invalidated the caller's token")
	}
	reloaded := &Node{StateDir: n.StateDir}
	if err := reloaded.load(); err != nil {
		t.Fatal(err)
	}
	if !reloaded.checkToken(token) {
		t.Fatal("after a restart the saved token no longer matches the caller's")
	}
}

// A boot attempt waiting on a tailnet login that never comes (the node was
// removed in the admin console, or the radio is offline) must not hold the
// operator's requests hostage: AetherSDR gives sign-out, the allowlist and
// shared devices 8 seconds.
func TestOperatorRequestsInterruptAStuckBoot(t *testing.T) {
	const token = "test-admin-token"
	n := newTestNode(t, token)
	n.applyRoutes = func([]netip.Prefix) error { return nil }
	attempts := make(chan struct{}, 16)
	n.startFn = func(ctx context.Context, authKey string, _ time.Duration) error {
		if authKey != "" {
			n.setState(stateRunning, "")
			return nil
		}
		attempts <- struct{}{}
		<-ctx.Done() // srv.Up waiting for a login
		n.setState(stateStarting, "")
		return ctx.Err()
	}
	waitAttempt := func(what string) {
		t.Helper()
		select {
		case <-attempts:
		case <-time.After(2 * time.Second):
			t.Fatalf("%s: no boot attempt", what)
		}
	}
	within := func(what string, f func() error) {
		t.Helper()
		done := make(chan error, 1)
		go func() { done <- f() }()
		select {
		case err := <-done:
			if err != nil {
				t.Fatalf("%s: %v", what, err)
			}
		case <-time.After(2 * time.Second):
			t.Fatalf("%s queued behind the boot attempt", what)
		}
	}

	n.Boot()
	waitAttempt("first boot")
	within("SetAllow", func() error { return n.SetAllow(token, []string{"tag:ops"}) })
	waitAttempt("boot after SetAllow") // a provisioned node keeps trying to rejoin
	within("SetSharing", func() error { return n.SetSharing(token, nil, nil) })
	waitAttempt("boot after SetSharing")
	// A caller without the token is refused without touching the boot
	// attempt: otherwise a LAN host could keep the radio off the tailnet by
	// restarting it in a loop.
	if !errors.Is(n.SetAllow("wrong", nil), errUnauthorized) ||
		!errors.Is(n.SetSharing("wrong", nil, nil), errUnauthorized) ||
		!errors.Is(n.SignOut("wrong"), errUnauthorized) {
		t.Fatal("a wrong token must still be refused")
	}
	if _, err := n.Provision("wrong", "tskey-auth-x", "flex-test", nil, nil, nil); !errors.Is(err, errUnauthorized) {
		t.Fatalf("re-provision with a wrong token: %v", err)
	}
	select {
	case <-attempts:
		t.Fatal("a refused request restarted the boot attempt")
	case <-time.After(200 * time.Millisecond):
	}

	var fresh string
	within("re-provision", func() (err error) {
		fresh, err = n.Provision(token, "tskey-auth-fresh", "flex-test", nil, nil, nil)
		return err
	})
	if !n.checkToken(fresh) || n.Status().State != stateRunning {
		t.Fatalf("re-provision did not take: %+v", n.Status())
	}
	within("sign-out", func() error { return n.SignOut(fresh) })
	if st := n.Status(); st.Provisioned || st.State != stateUnprovisioned {
		t.Fatalf("sign-out left %+v", st)
	}
}

// Sign-out on its own, while the boot attempt is parked waiting for a login.
func TestSignOutInterruptsAStuckBoot(t *testing.T) {
	const token = "test-admin-token"
	n := newTestNode(t, token)
	attempts := make(chan struct{}, 4)
	n.startFn = func(ctx context.Context, _ string, _ time.Duration) error {
		attempts <- struct{}{}
		<-ctx.Done()
		return ctx.Err()
	}
	n.Boot()
	select {
	case <-attempts:
	case <-time.After(2 * time.Second):
		t.Fatal("no boot attempt")
	}
	done := make(chan error, 1)
	go func() { done <- n.SignOut(token) }()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("sign-out queued behind the boot attempt")
	}
	if st := n.Status(); st.Provisioned || st.State != stateUnprovisioned {
		t.Fatalf("sign-out left %+v", st)
	}
	select {
	case <-attempts:
		t.Fatal("a signed-out node kept booting")
	case <-time.After(300 * time.Millisecond):
	}
}

// Saving a narrower allowlist ends what it no longer admits, at once: the
// removed computer's radio session (so the radio drops it and unkeys what it
// owned), its side-channel transfer and its station-device splice. The
// computer still allowed keeps its session.
func TestSaveAccessListEndsRemovedConnections(t *testing.T) {
	const token = "test-admin-token"
	n := newTestNode(t, token)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	sessionFrom := func(id uint64, ip net.IP) *Session {
		d := net.Dialer{LocalAddr: &net.TCPAddr{IP: ip}}
		cl, err := d.Dial("tcp", ln.Addr().String())
		if err != nil {
			t.Skipf("needs %v on loopback: %v", ip, err)
		}
		srv, err := ln.Accept()
		if err != nil {
			t.Fatal(err)
		}
		radioA, radioB := net.Pipe()
		t.Cleanup(func() { cl.Close(); srv.Close(); radioA.Close(); radioB.Close() })
		return &Session{id: id, client: srv, radio: radioA, host: loopUDP(t),
			clientIP: netip.MustParseAddr(ip.String()), done: make(chan struct{})}
	}
	r := &Relay{}
	r.init()
	kept, removed := sessionFrom(1, net.IPv4(127, 0, 0, 1)), sessionFrom(2, net.IPv4(127, 0, 0, 2))
	for _, s := range []*Session{kept, removed} {
		s.relay = r
		r.sessions[s.id] = s
	}
	n.relay = r
	n.fwd = &Forwarder{}
	n.authorize = func(a net.Addr) (string, bool) {
		ap, _ := addrPortOf(a)
		return ap.Addr().String(), ap.Addr() == netip.MustParseAddr("127.0.0.1")
	}
	removedFrom := &net.TCPAddr{IP: net.IPv4(127, 0, 0, 2), Port: 1}
	devA, devB := net.Pipe()
	sideA, sideB := net.Pipe()
	defer devB.Close()
	defer sideB.Close()
	n.splices.add(removedFrom, netip.MustParseAddr("192.168.50.103"), devA)
	n.fwd.splices.add(removedFrom, netip.Addr{}, sideA)

	if err := n.SetAllow(token, []string{"tag:ops"}); err != nil {
		t.Fatal(err)
	}
	if !removed.waitClosed(2 * time.Second) {
		t.Fatal("the removed computer's radio session is still open")
	}
	if kept.waitClosed(100 * time.Millisecond) {
		t.Fatal("the allowed computer's session was ended")
	}
	for name, c := range map[string]net.Conn{"device splice": devB, "side channel": sideB} {
		c.SetReadDeadline(time.Now().Add(2 * time.Second))
		if _, err := c.Read(make([]byte, 1)); err == nil || errors.Is(err, os.ErrDeadlineExceeded) {
			t.Fatalf("the removed computer's %s is still open: %v", name, err)
		}
	}
}

// If a cancelled boot attempt is slow to unwind (tsnet teardown), an
// operator request answers "try again" inside AetherSDR's timeout instead of
// hanging, and the boot resumes by itself afterwards.
func TestSlowBootTeardownAnswersBusy(t *testing.T) {
	const token = "test-admin-token"
	n := newTestNode(t, token)
	n.interruptWait = 100 * time.Millisecond
	attempts := make(chan struct{}, 4)
	n.startFn = func(ctx context.Context, _ string, _ time.Duration) error {
		attempts <- struct{}{}
		<-ctx.Done()
		time.Sleep(400 * time.Millisecond) // teardown outlasting the wait
		return ctx.Err()
	}
	n.Boot()
	select {
	case <-attempts:
	case <-time.After(2 * time.Second):
		t.Fatal("no boot attempt")
	}
	start := time.Now()
	err := n.SetAllow(token, []string{"tag:ops"})
	if !errors.Is(err, errBusy) || time.Since(start) > time.Second {
		t.Fatalf("SetAllow: %v after %v, want errBusy promptly", err, time.Since(start))
	}
	select {
	case <-attempts: // resumed once the slow attempt was gone
	case <-time.After(3 * time.Second):
		t.Fatal("boot didn't resume after the slow teardown")
	}
	n.mu.Lock()
	n.cfg = provisionConfig{} // stop the loop
	n.mu.Unlock()
	n.interruptBoot()
}

// Revocation acts only on a definite refusal: a WhoIs that couldn't answer
// keeps the session, so a tailscaled hiccup during Save Access List doesn't
// cut off operators still on the list.
func TestRevocationKeepsSessionsWhoseIdentityIsUnavailable(t *testing.T) {
	r := &Relay{}
	r.init()
	a, b := net.Pipe()
	ra, rb := net.Pipe()
	defer a.Close()
	defer b.Close()
	defer ra.Close()
	defer rb.Close()
	s := &Session{id: 1, relay: r, client: a, radio: ra, host: loopUDP(t),
		clientIP: netip.MustParseAddr("100.64.0.9"), done: make(chan struct{})}
	r.sessions[1] = s
	n := &Node{relay: r, fwd: &Forwarder{}}
	n.authorize = func(net.Addr) (string, bool) { return "unknown peer", false }
	n.check = func(net.Addr) (string, bool, error) { return "", false, errors.New("whois timed out") }
	n.revokeDisallowed()
	if s.waitClosed(200 * time.Millisecond) {
		t.Fatal("a session was ended because its identity couldn't be checked")
	}
	n.check = func(net.Addr) (string, bool, error) { return "guest on phone", false, nil }
	n.revokeDisallowed()
	if !s.waitClosed(2 * time.Second) {
		t.Fatal("a definite refusal didn't end the session")
	}
}

// A retry after errBusy, while the cancelled attempt is still unwinding,
// is answered the same bounded way instead of queuing behind it.
func TestRetryWhileBootStillUnwindingAnswersBusy(t *testing.T) {
	const token = "test-admin-token"
	n := newTestNode(t, token)
	n.interruptWait = 100 * time.Millisecond
	attempts := make(chan struct{}, 4)
	n.startFn = func(ctx context.Context, _ string, _ time.Duration) error {
		attempts <- struct{}{}
		<-ctx.Done()
		time.Sleep(600 * time.Millisecond)
		return ctx.Err()
	}
	n.Boot()
	<-attempts
	for i := 0; i < 2; i++ {
		res := make(chan error, 1)
		go func() { res <- n.SetAllow(token, nil) }()
		select {
		case err := <-res:
			if !errors.Is(err, errBusy) {
				t.Fatalf("request %d: %v, want errBusy", i+1, err)
			}
		case <-time.After(400 * time.Millisecond):
			t.Fatalf("request %d queued behind the unwinding boot attempt", i+1)
		}
	}
	select {
	case <-attempts:
	case <-time.After(3 * time.Second):
		t.Fatal("boot didn't resume")
	}
	n.mu.Lock()
	n.cfg = provisionConfig{}
	n.mu.Unlock()
	n.interruptBoot()
}

// Withdrawing a route ends connections already open to that device.
func TestUnsharingADeviceEndsItsConnections(t *testing.T) {
	n := &Node{advertised: []netip.Prefix{netip.MustParsePrefix("192.168.50.103/32"), netip.MustParsePrefix("192.168.50.101/32")}}
	n.applyRoutes = func([]netip.Prefix) error { return nil }
	from := &net.TCPAddr{IP: net.IPv4(100, 64, 0, 9), Port: 1}
	agA, agB := net.Pipe()
	tgA, tgB := net.Pipe()
	defer agB.Close()
	defer tgB.Close()
	n.splices.add(from, netip.MustParseAddr("192.168.50.103"), agA)
	n.splices.add(from, netip.MustParseAddr("192.168.50.101"), tgA)
	off := false
	n.cfg = provisionConfig{Routes: []string{"192.168.50.101"}, ShareDiscovered: &off}
	n.RefreshRoutes()
	agB.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := agB.Read(make([]byte, 1)); err == nil || errors.Is(err, os.ErrDeadlineExceeded) {
		t.Fatalf("a connection to an unshared device stayed open: %v", err)
	}
	tgB.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	if _, err := tgB.Read(make([]byte, 1)); !errors.Is(err, os.ErrDeadlineExceeded) {
		t.Fatalf("a connection to a still-shared device was closed: %v", err)
	}
}
