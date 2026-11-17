package main

import (
	"context"
	"errors"
	"net/http"
	"os"
	"strings"
	"testing"
	"time"
)

const managedOwnerFixture = "managed-owner-fixture"

func managedIdentityFixture(t *testing.T) *Node {
	t.Helper()
	n := newTestNode(t, managedOwnerFixture)
	n.RunDir = t.TempDir()
	n.cfg.LocalOnly = true
	n.cfg.InstallationID = strings.Repeat("ab", 32)
	if err := n.save(n.cfg); err != nil {
		t.Fatal(err)
	}
	if err := n.load(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if _, err := n.interruptBoot(); err != nil {
			t.Error(err)
		}
	})
	return n
}

func assertManagedOwner(t *testing.T, n *Node) {
	t.Helper()
	if status := n.Status(); !status.Provisioned || status.Claimable || status.InstallationID != strings.Repeat("ab", 32) || status.ManagedOwnerVersion != 1 {
		t.Fatalf("managed installation lost its owner boundary: %+v", status)
	}
	if !n.checkToken(managedOwnerFixture) || n.checkToken("foreign-owner") {
		t.Fatal("managed owner credential changed")
	}
}

func TestManagedEnrollmentKeepsInstallationOwnerAfterLostReply(t *testing.T) {
	for _, interactive := range []bool{false, true} {
		t.Run(map[bool]string{false: "auth-key", true: "interactive"}[interactive], func(t *testing.T) {
			n := managedIdentityFixture(t)
			body := `{"hostname":"managed-radio","auth_key":"tskey-auth-fixture"}`
			n.startFn = inertStart(n)
			if interactive {
				body = `{"hostname":"managed-radio"}`
				n.startFn = pendingSignInStart(n)
			}
			// Discard the provision response entirely. Recovery must use only
			// the pre-existing keychain token, including after a process restart.
			rec := call(t, NewAPI(n), "POST", "/v1/provision", "192.168.50.2:4000", managedOwnerFixture, body)
			if rec.Code != http.StatusOK {
				t.Fatalf("enrollment HTTP %d", rec.Code)
			}
			assertManagedOwner(t, n)
			loaded := credentialReload(t, n)
			assertManagedOwner(t, loaded)
			if _, err := n.Login(managedOwnerFixture); err != nil {
				t.Fatal("lost reply stranded existing owner", err)
			}
			if _, err := loaded.Login(managedOwnerFixture); err != nil {
				t.Fatal("restart stranded existing owner", err)
			}
			if interactive {
				for _, presented := range []string{"", "foreign-owner"} {
					if _, _, err := n.ProvisionInteractive(presented, "foreign-radio", nil, nil, nil); !errors.Is(err, errUnauthorized) {
						t.Fatal("pending managed sign-in admitted a foreign replacement", err)
					}
				}
			}
		})
	}
}

func TestManagedRejectedKeyReturnsToLocalOwner(t *testing.T) {
	n := managedIdentityFixture(t)
	n.startFn = func(context.Context, string, time.Duration) error { return errors.New("fixture rejected key") }
	if _, err := n.Provision(managedOwnerFixture, "tskey-auth-fixture", "managed-radio", nil, nil, nil); err == nil {
		t.Fatal("rejected key was reported as successful")
	}
	for _, node := range []*Node{n, credentialReload(t, n)} {
		assertManagedOwner(t, node)
		if node.Status().State != stateLocal {
			t.Fatal("failed enrollment did not return to local control")
		}
	}
}

func TestManagedDisconnectKeepsLocalControlAfterLostReply(t *testing.T) {
	n := managedIdentityFixture(t)
	n.cfg.LocalOnly = false
	n.cfg.PendingSignIn = true
	if err := n.save(n.cfg); err != nil {
		t.Fatal(err)
	}
	writeTailnetState(t, n.tsnetDir(), map[string]string{"fixture": "private-state"})
	rec := call(t, NewAPI(n), "POST", "/v1/signout", "192.168.50.2:4000", managedOwnerFixture, "")
	if rec.Code != http.StatusOK {
		t.Fatalf("disconnect HTTP %d", rec.Code)
	}
	for _, node := range []*Node{n, credentialReload(t, n)} {
		assertManagedOwner(t, node)
		node.startFn = func(context.Context, string, time.Duration) error {
			t.Error("local control restarted tailnet enrollment")
			return nil
		}
		node.Boot()
		if node.Status().State != stateLocal {
			t.Fatal("disconnect did not retain local control")
		}
	}
	if _, err := os.Stat(n.tsnetDir()); !os.IsNotExist(err) {
		t.Fatal("disconnect kept tailnet identity", err)
	}
}

func TestManagedDisconnectWriteFailurePreservesOwner(t *testing.T) {
	n := managedIdentityFixture(t)
	n.cfg.LocalOnly = false
	if err := n.save(n.cfg); err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadFile(n.cfgPath())
	if err != nil {
		t.Fatal(err)
	}
	n.configWrite = func([]byte) error { return errors.New("fixture full disk") }
	if err := n.SignOut(managedOwnerFixture); err == nil {
		t.Fatal("failed local-control save reported a successful disconnect")
	}
	assertManagedOwner(t, n)
	after, err := os.ReadFile(n.cfgPath())
	if err != nil || string(after) != string(before) {
		t.Fatal("failed disconnect changed persisted owner", err)
	}
}

func TestManagedTailnetIdentityValidationDoesNotDependOnLocalOnly(t *testing.T) {
	for _, id := range []string{"ab", strings.Repeat("AB", 32), strings.Repeat("0", 64)} {
		t.Run(id, func(t *testing.T) {
			n := managedIdentityFixture(t)
			n.cfg.LocalOnly = false
			n.cfg.InstallationID = id
			if err := n.save(n.cfg); err != nil {
				t.Fatal(err)
			}
			loaded := &Node{StateDir: n.StateDir, RunDir: n.RunDir}
			if err := loaded.load(); err == nil || loaded.Status().Claimable || loaded.checkToken(managedOwnerFixture) {
				t.Fatal("invalid managed tailnet identity became usable or claimable")
			}
		})
	}
}

func TestManagedDisconnectDiscardsDelayedLoginAndJoinSave(t *testing.T) {
	n := managedIdentityFixture(t)
	n.cfg.LocalOnly = false
	n.cfg.PendingSignIn = true
	if err := n.save(n.cfg); err != nil {
		t.Fatal(err)
	}
	gen := n.joinGen
	entered, release := make(chan struct{}), make(chan struct{})
	n.loginRead = func(context.Context) (loginInfo, error) {
		close(entered)
		<-release
		return loginInfo{Backend: "NeedsLogin", AuthURL: testAuthURL}, nil
	}
	type loginResult struct {
		reply LoginReply
		err   error
	}
	done := make(chan loginResult, 1)
	go func() {
		reply, err := n.Login(managedOwnerFixture)
		done <- loginResult{reply, err}
	}()
	waitCredentialSignal(t, entered)
	if err := n.SignOut(managedOwnerFixture); err != nil {
		close(release)
		t.Fatal(err)
	}
	close(release)
	select {
	case result := <-done:
		if result.err != nil || result.reply.State != stateLocal || result.reply.AuthURL != "" {
			t.Fatal("delayed tailnet URL outlived managed disconnect", result.err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("login did not finish")
	}
	n.markJoined(gen)
	assertManagedOwner(t, credentialReload(t, n))
	if !n.cfg.LocalOnly || n.cfg.PendingSignIn {
		t.Fatal("old join callback reopened enrollment")
	}
}
