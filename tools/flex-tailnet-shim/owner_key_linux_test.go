//go:build linux

package main

import (
	"context"
	"errors"
	"os"
	"testing"
)

func TestOwnerKeyNativeCommitBoundaries(t *testing.T) {
	for _, failure := range []string{"parent-before", "file-sync", "rename", "parent-after", ""} {
		t.Run("failure="+failure, func(t *testing.T) {
			n := managedIdentityFixture(t)
			channel, done := pdSessionTestPair(t, func(context.Context, byte) pdSessionReply { return pdSessionReply{} })
			n.pdSession = channel
			ops, _ := recordingNativeCommitOps(t, failure)
			n.configWrite = func(raw []byte) error { return writeNativeStateFileWithOps(n.cfgPath(), raw, ops) }
			_, err := n.replaceOwnerKey(managedOwnerFixture, ownerKeyFixtureRequest(n))
			if (err == nil) != (failure == "") || errors.Is(err, errNativeCommitUncertain) != (failure == "parent-after") {
				t.Fatal("incorrect native persistence result", err)
			}
			selected := credentialReload(t, n)
			committed := failure == "" || failure == "parent-after"
			if selected.checkToken(ownerKeyCandidate) != committed || selected.checkToken(managedOwnerFixture) == committed {
				t.Fatal("restart selected wrong credential at commit boundary")
			}
			if failure == "parent-after" {
				if !n.nativeStateUnavailable || n.checkToken(managedOwnerFixture) || n.checkToken(ownerKeyCandidate) || n.Status().OwnerKeyVersion != 0 || n.Status().Claimable {
					t.Fatal("uncertain commit retained authority or reopened first use")
				}
				if _, err := n.replaceOwnerKey(ownerKeyCandidate, ownerKeyFixtureRequest(n)); !errors.Is(err, errUnauthorized) {
					t.Fatal("uncertain process admitted replacement-key retry", err)
				}
			} else if failure != "" {
				assertManagedOwner(t, n)
			}
			if channel.alive() == committed || n.ownerKeyChanging {
				t.Fatal("incorrect authority retirement at native commit boundary")
			}
			if committed {
				waitCredentialSignal(t, done)
			}
		})
	}
}

func TestOwnerKeyDirectProcessAuthority(t *testing.T) {
	for _, uncertain := range []bool{false, true} {
		t.Run(map[bool]string{false: "committed", true: "uncertain"}[uncertain], func(t *testing.T) {
			n := managedIdentityFixture(t)
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
			if uncertain {
				ops, _ := recordingNativeCommitOps(t, "parent-after")
				n.configWrite = func(raw []byte) error { return writeNativeStateFileWithOps(n.cfgPath(), raw, ops) }
			}
			_, err = n.replaceOwnerKey(managedOwnerFixture, ownerKeyFixtureRequest(n))
			if (err != nil) != uncertain {
				t.Fatal(err)
			}
			assertPDAuthorityRevoked(t, p, ctx)
		})
	}
}
