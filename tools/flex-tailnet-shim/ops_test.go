package main

import (
	"bufio"
	"errors"
	"io"
	"net/netip"
	"os"
	"strings"
	"sync"
	"testing"
	"time"
)

// Socket-free regressions for the Node's serialized operations: no tsnet,
// no radio. startFn and applyRoutes stand in for the tailnet.

func inertStart(n *Node) func(string, time.Duration) error {
	return func(string, time.Duration) error {
		n.setState(stateRunning, "")
		return nil
	}
}

// Two first-use provisions race: only the first may succeed without a token.
func TestConcurrentFirstUseProvisionNeedsTheFirstToken(t *testing.T) {
	n := newTestNode(t, "")
	release := make(chan struct{})
	entered := make(chan struct{}, 2)
	n.startFn = func(string, time.Duration) error {
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
