package main

import (
	"context"
	"errors"
	"os"
	"runtime"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func credentialFixture(t *testing.T, native bool) *Node {
	t.Helper()
	n := &Node{StateDir: t.TempDir(), state: stateStarting}
	if native {
		n.RunDir = t.TempDir()
	}
	n.cfg = provisionConfig{TokenSHA256: sha256Hex("old-owner-fixture"), Hostname: "old-radio", PendingSignIn: true}
	if err := n.save(n.cfg); err != nil {
		t.Fatal(err)
	}
	return n
}
func waitCredentialSignal(t *testing.T, ch <-chan struct{}) {
	t.Helper()
	select {
	case <-ch:
	case <-time.After(3 * time.Second):
		t.Fatal("credential operation did not reach barrier")
	}
}
func waitCredentialResult(t *testing.T, ch <-chan error) {
	t.Helper()
	select {
	case err := <-ch:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("credential operation did not finish")
	}
}
func pauseCredentialWrite(n *Node) (<-chan struct{}, func()) {
	entered, release := make(chan struct{}), make(chan struct{})
	var calls atomic.Uint32
	n.configWrite = func(data []byte) error {
		if calls.Add(1) == 1 {
			close(entered)
			<-release
		}
		return n.writeConfigFile(data)
	}
	return entered, func() { close(release) }
}
func credentialReload(t *testing.T, n *Node) *Node {
	t.Helper()
	loaded := &Node{StateDir: n.StateDir, RunDir: n.RunDir}
	if err := loaded.load(); err != nil {
		t.Fatal(err)
	}
	return loaded
}

func TestCredentialJoinedSaveCannotRecreateSignedOutIdentity(t *testing.T) {
	for _, native := range []bool{false, true} {
		t.Run(map[bool]string{false: "container", true: "native"}[native], func(t *testing.T) {
			n := credentialFixture(t, native)
			entered, release := pauseCredentialWrite(n)
			joinDone := make(chan struct{})
			go func() { n.markJoined(0); close(joinDone) }()
			waitCredentialSignal(t, entered)
			revoked := make(chan struct{})
			n.closers = []func() error{func() error { close(revoked); return nil }}
			signout := make(chan error, 1)
			go func() { signout <- n.SignOut("old-owner-fixture") }()
			// Drive the old failure ordering if deletion can pass the paused writer.
			// Correct serialization waits here; final file/token assertions are decisive.
			select {
			case <-revoked:
			case <-time.After(50 * time.Millisecond):
			}
			release()
			waitCredentialSignal(t, joinDone)
			waitCredentialResult(t, signout)
			if _, err := os.Stat(n.cfgPath()); !os.IsNotExist(err) {
				t.Fatal("old join save recreated signed-out configuration")
			}
			loaded := credentialReload(t, n)
			if n.checkToken("old-owner-fixture") || loaded.checkToken("old-owner-fixture") || loaded.Status().Provisioned {
				t.Fatal("revoked identity survived in memory or after reload")
			}
		})
	}
}

func TestCredentialJoinedSaveCannotOverwriteReplacement(t *testing.T) {
	for _, native := range []bool{false, true} {
		t.Run(map[bool]string{false: "container", true: "native"}[native], func(t *testing.T) {
			n := credentialFixture(t, native)
			entered, release := pauseCredentialWrite(n)
			done := make(chan struct{})
			go func() { n.markJoined(0); close(done) }()
			waitCredentialSignal(t, entered)
			replacement := make(chan error, 1)
			replacing := make(chan struct{})
			go func() {
				n.opMu.Lock()
				close(replacing)
				defer n.opMu.Unlock()
				replacement <- n.replaceNode(provisionConfig{TokenSHA256: sha256Hex("new-owner-fixture"), Hostname: "new-radio", PendingSignIn: true})
			}()
			waitCredentialSignal(t, replacing)
			select {
			case err := <-replacement:
				replacement <- err
			case <-time.After(50 * time.Millisecond):
			}
			release()
			waitCredentialSignal(t, done)
			waitCredentialResult(t, replacement)
			n.markJoined(0) // delayed old callback delivered after the new owner committed
			loaded := credentialReload(t, n)
			if !n.cfg.PendingSignIn || loaded.checkToken("old-owner-fixture") || !loaded.checkToken("new-owner-fixture") || !loaded.cfg.PendingSignIn || loaded.cfg.Hostname != "new-radio" {
				t.Fatal("old joined callback changed replacement identity")
			}
			n.mu.Lock()
			gen := n.joinGen
			n.mu.Unlock()
			n.markJoined(gen)
			loaded = credentialReload(t, n)
			if loaded.cfg.PendingSignIn || !loaded.checkToken("new-owner-fixture") {
				t.Fatal("current join did not persist completion")
			}
		})
	}
}

func TestCredentialSharingAndJoinRetainBothChanges(t *testing.T) {
	for _, native := range []bool{false, true} {
		t.Run(map[bool]string{false: "container", true: "native"}[native], func(t *testing.T) {
			n := credentialFixture(t, native)
			entered, release := pauseCredentialWrite(n)
			off := false
			sharing := make(chan error, 1)
			go func() { sharing <- n.SetSharing("old-owner-fixture", []string{"192.168.50.20"}, &off) }()
			waitCredentialSignal(t, entered)
			done := make(chan struct{})
			go func() { n.markJoined(0); close(done) }()
			deadline := time.Now().Add(3 * time.Second)
			for n.Status().Claimable {
				if time.Now().After(deadline) {
					release()
					t.Fatal("completed sign-in remained claimable behind a pending sharing write")
				}
				runtime.Gosched()
			}
			select {
			case <-done:
			case <-time.After(50 * time.Millisecond):
			}
			release()
			waitCredentialResult(t, sharing)
			waitCredentialSignal(t, done)
			loaded := credentialReload(t, n)
			if loaded.cfg.PendingSignIn || len(loaded.cfg.Routes) != 1 || loaded.cfg.Routes[0] != "192.168.50.20" || loaded.cfg.shareDiscovered() {
				t.Fatal("sharing or joined state lost during serialized commits")
			}
		})
	}
}

func TestCredentialStaleWatchJoinDoesNotChangeReplacement(t *testing.T) {
	oldPoll := loginPoll
	loginPoll = time.Millisecond
	defer func() { loginPoll = oldPoll }()
	for _, backend := range []string{"NeedsMachineAuth", "NeedsLogin"} {
		t.Run(backend, func(t *testing.T) {
			n := credentialFixture(t, true)
			entered, release := make(chan struct{}), make(chan struct{})
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			var readOnce sync.Once
			read := func(context.Context) (loginInfo, error) {
				readOnce.Do(func() { close(entered); <-release })
				return loginInfo{Backend: backend, AuthURL: "https://login.tailscale.com/a/fixture"}, nil
			}
			done := make(chan struct{})
			go func() { n.watchJoin(ctx, 0, read, true, func() {}); close(done) }()
			waitCredentialSignal(t, entered)
			n.opMu.Lock()
			err := n.replaceNode(provisionConfig{TokenSHA256: sha256Hex("new-owner-fixture"), Hostname: "new-radio", PendingSignIn: true})
			n.opMu.Unlock()
			if err != nil {
				t.Fatal(err)
			}
			n.setState(stateStarting, "")
			cancel()
			close(release)
			waitCredentialSignal(t, done)
			loaded := credentialReload(t, n)
			if n.Status().State != stateStarting || !n.cfg.PendingSignIn || !loaded.cfg.PendingSignIn || !loaded.checkToken("new-owner-fixture") {
				t.Fatal("old watcher changed successor state or ownership")
			}
		})
	}
}

func TestCredentialLoginReadCannotOutliveRevocation(t *testing.T) {
	for _, replace := range []bool{false, true} {
		t.Run(map[bool]string{false: "signout", true: "replace"}[replace], func(t *testing.T) {
			n := credentialFixture(t, true)
			entered, release := make(chan struct{}), make(chan struct{})
			n.loginRead = func(context.Context) (loginInfo, error) {
				close(entered)
				<-release
				return loginInfo{Backend: "NeedsLogin", AuthURL: "https://login.tailscale.com/a/fixture-private-url"}, nil
			}
			result := make(chan error, 1)
			go func() {
				r, err := n.Login("old-owner-fixture")
				if r.AuthURL != "" {
					result <- errors.New("revoked credential received delayed login URL")
					return
				}
				result <- err
			}()
			waitCredentialSignal(t, entered)
			if replace {
				n.opMu.Lock()
				err := n.replaceNode(provisionConfig{TokenSHA256: sha256Hex("new-owner-fixture"), Hostname: "new-radio", PendingSignIn: true})
				n.opMu.Unlock()
				if err != nil {
					t.Fatal(err)
				}
			} else {
				if err := n.SignOut("old-owner-fixture"); err != nil {
					t.Fatal(err)
				}
			}
			close(release)
			select {
			case err := <-result:
				if !errors.Is(err, errUnauthorized) {
					t.Fatalf("delayed login authorization: %v", err)
				}
			case <-time.After(3 * time.Second):
				t.Fatal("login read did not finish")
			}
		})
	}
}

func TestCredentialFailedReplacementDoesNotRejectCurrentOwner(t *testing.T) {
	n := credentialFixture(t, true)
	n.cfg.PendingSignIn = false
	if err := n.save(n.cfg); err != nil {
		t.Fatal(err)
	}
	entered, release := make(chan struct{}), make(chan struct{})
	n.loginRead = func(context.Context) (loginInfo, error) {
		close(entered)
		<-release
		return loginInfo{Backend: "NeedsLogin", AuthURL: "https://login.tailscale.com/a/stale-fixture"}, nil
	}
	type outcome struct {
		reply LoginReply
		err   error
	}
	done := make(chan outcome, 1)
	go func() { reply, err := n.Login("old-owner-fixture"); done <- outcome{reply, err} }()
	waitCredentialSignal(t, entered)
	n.configWrite = func([]byte) error { return os.ErrPermission }
	n.opMu.Lock()
	err := n.replaceNode(provisionConfig{TokenSHA256: sha256Hex("new-owner-fixture"), Hostname: "new-radio"})
	n.opMu.Unlock()
	if err == nil {
		t.Fatal("replacement unexpectedly persisted")
	}
	close(release)
	select {
	case result := <-done:
		if result.err != nil || result.reply.AuthURL != "" || result.reply.State != stateError {
			t.Fatalf("current owner lost authorization or received stale sign-in: %v state=%q", result.err, result.reply.State)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("login did not finish")
	}
	if !n.checkToken("old-owner-fixture") || !credentialReload(t, n).checkToken("old-owner-fixture") {
		t.Fatal("failed replacement changed owner")
	}
}
