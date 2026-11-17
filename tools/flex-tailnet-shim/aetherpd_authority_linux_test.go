//go:build linux

package main

import (
	"context"
	"errors"
	"os"
	"strings"
	"testing"
	"time"
)

func pdAuthorityFixture(t *testing.T) (*Node, *aetherPDProcess, context.Context) {
	t.Helper()
	n := newTestNode(t, "current-admin")
	n.startFn = inertStart(n)
	read, write, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { read.Close(); write.Close() })
	p := &aetherPDProcess{owner: write}
	n.pdProcess = p
	ctx, finish, err := p.calibrationIntents.beginArm(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(finish)
	return n, p, ctx
}
func assertPDAuthorityRevoked(t *testing.T, p *aetherPDProcess, ctx context.Context) {
	t.Helper()
	if ctx.Err() != context.Canceled {
		t.Fatal("authority change retained a pending calibration arm")
	}
	if _, err := p.owner.Stat(); !errors.Is(err, os.ErrClosed) {
		t.Fatal("authority change retained the DSP lifetime writer", err)
	}
	if _, _, err := p.calibrationIntents.beginArm(context.Background()); !errors.Is(err, errPDCalibrationStopped) {
		t.Fatal("revoked process did not permanently close calibration", err)
	}
}
func assertPDAuthorityUnchanged(t *testing.T, p *aetherPDProcess, ctx context.Context) {
	t.Helper()
	if ctx.Err() != nil {
		t.Fatal("rejected authority change canceled the current arm")
	}
	if _, err := p.owner.Stat(); err != nil {
		t.Fatal("rejected authority change stopped the current DSP", err)
	}
}
func TestPDTokenRotationStopsOnlyOwnedSession(t *testing.T) {
	n, p, ctx := pdAuthorityFixture(t)
	token, err := n.Provision("current-admin", "tskey-auth-fixture", "new-owner", nil, nil, nil)
	if err != nil || token == "" {
		t.Fatal(err)
	}
	assertPDAuthorityRevoked(t, p, ctx)
	if n.checkToken("current-admin") || !n.checkToken(token) {
		t.Fatal("token rotation not committed")
	}
}
func TestPDSignOutRevokesBeforeSlowLogout(t *testing.T) {
	n, p, ctx := pdAuthorityFixture(t)
	entered, release := make(chan struct{}), make(chan struct{})
	n.closers = []func() error{func() error { close(entered); <-release; return nil }}
	done := make(chan error, 1)
	go func() { done <- n.SignOut("current-admin") }()
	defer func() {
		close(release)
		select {
		case err := <-done:
			if err != nil {
				t.Error(err)
			}
		case <-time.After(time.Second):
			t.Error("signout goroutine did not finish")
		}
	}()
	select {
	case <-entered:
	case <-time.After(time.Second):
		t.Fatal("signout did not reach logout")
	}
	assertPDAuthorityRevoked(t, p, ctx)
	if n.checkToken("current-admin") {
		t.Fatal("slow logout kept the revoked token valid")
	}
}
func TestPDRejectedAuthorityChangePreservesSession(t *testing.T) {
	n, p, ctx := pdAuthorityFixture(t)
	if _, err := n.Provision("wrong", "tskey-auth-fixture", "new-owner", nil, nil, nil); !errors.Is(err, errUnauthorized) {
		t.Fatal(err)
	}
	assertPDAuthorityUnchanged(t, p, ctx)
	if err := n.SignOut("wrong"); !errors.Is(err, errUnauthorized) {
		t.Fatal(err)
	}
	assertPDAuthorityUnchanged(t, p, ctx)
}
func TestPDFailedAuthorityPersistencePreservesSession(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("root ignores directory permissions")
	}
	n, p, ctx := pdAuthorityFixture(t)
	if err := os.Chmod(n.StateDir, 0o500); err != nil {
		t.Fatal(err)
	}
	defer os.Chmod(n.StateDir, 0o700)
	if _, err := n.Provision("current-admin", "tskey-auth-fixture", "new-owner", nil, nil, nil); err == nil {
		t.Fatal("unwritable replacement succeeded")
	}
	assertPDAuthorityUnchanged(t, p, ctx)
	if err := n.SignOut("current-admin"); err == nil {
		t.Fatal("undeletable signout succeeded")
	}
	assertPDAuthorityUnchanged(t, p, ctx)
	if !n.checkToken("current-admin") {
		t.Fatal("failed persistence revoked old token")
	}
}

func TestPDNonAuthorityConfigUpdatePreservesSession(t *testing.T) {
	n, p, ctx := pdAuthorityFixture(t)
	if err := n.SetAllow("current-admin", nil); err != nil {
		t.Fatal(err)
	}
	assertPDAuthorityUnchanged(t, p, ctx)
}

func TestPDManagedRejoinRetiresSessionWithoutRotatingOwner(t *testing.T) {
	n, p, ctx := pdAuthorityFixture(t)
	n.cfg.InstallationID = strings.Repeat("ab", 32)
	if err := n.save(n.cfg); err != nil {
		t.Fatal(err)
	}
	token, err := n.Provision("current-admin", "tskey-auth-fixture", "managed-radio", nil, nil, nil)
	if err != nil || token != "current-admin" {
		t.Fatal("managed rejoin changed its local owner", err)
	}
	assertPDAuthorityRevoked(t, p, ctx)
}

func TestPDManagedDisconnectRetiresBeforeSlowLogout(t *testing.T) {
	n, p, ctx := pdAuthorityFixture(t)
	n.cfg.InstallationID = strings.Repeat("ab", 32)
	if err := n.save(n.cfg); err != nil {
		t.Fatal(err)
	}
	entered, release := make(chan struct{}), make(chan struct{})
	n.closers = []func() error{func() error { close(entered); <-release; return nil }}
	done := make(chan error, 1)
	go func() { done <- n.SignOut("current-admin") }()
	defer func() {
		close(release)
		waitCredentialResult(t, done)
	}()
	waitCredentialSignal(t, entered)
	assertPDAuthorityRevoked(t, p, ctx)
	if !n.checkToken("current-admin") || n.Status().State != stateLocal {
		t.Fatal("disconnect stranded local control during slow logout")
	}
}

func TestPDManagedFailedDisconnectWriteKeepsSession(t *testing.T) {
	n, p, ctx := pdAuthorityFixture(t)
	n.cfg.InstallationID = strings.Repeat("ab", 32)
	if err := n.save(n.cfg); err != nil {
		t.Fatal(err)
	}
	n.configWrite = func([]byte) error { return errors.New("fixture full disk") }
	if err := n.SignOut("current-admin"); err == nil {
		t.Fatal("failed write reported successful disconnect")
	}
	assertPDAuthorityUnchanged(t, p, ctx)
}
