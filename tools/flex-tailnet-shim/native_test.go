package main

import (
	"os"
	"path/filepath"
	"testing"

	"tailscale.com/ipn"
)

// With a RunDir, only the node's identity is written under StateDir; tsnet's
// logs and caches go to the run directory.
func TestRunDirKeepsOnlyTheIdentityInState(t *testing.T) {
	state, run := t.TempDir(), t.TempDir()
	n := &Node{StateDir: state, RunDir: run}
	srv, err := n.newTsnetServer("radio", "")
	if err != nil {
		t.Fatal(err)
	}
	if want := filepath.Join(run, "tsnet"); srv.Dir != want {
		t.Fatalf("tsnet dir %q, want %q", srv.Dir, want)
	}
	if srv.Store == nil {
		t.Fatal("no state store: the identity would land in the run directory")
	}
	if err := srv.Store.WriteState(ipn.StateKey("_machinekey"), []byte("k")); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(state, "tsnet", "tailscaled.state")); err != nil {
		t.Fatalf("identity not in the state directory: %v", err)
	}
	if ents, _ := os.ReadDir(filepath.Join(run, "tsnet")); len(ents) != 0 {
		t.Fatalf("the run directory holds %d files before tsnet starts", len(ents))
	}
}

// Without a RunDir everything stays where the container build keeps it, so
// existing radios keep their node.
func TestNoRunDirIsTheContainerLayout(t *testing.T) {
	state := t.TempDir()
	n := &Node{StateDir: state}
	srv, err := n.newTsnetServer("radio", "")
	if err != nil {
		t.Fatal(err)
	}
	if srv.Dir != filepath.Join(state, "tsnet") || srv.Store != nil {
		t.Fatalf("dir %q store %v", srv.Dir, srv.Store)
	}
}

// Signing out deletes the identity and the run directory's logs alike.
func TestSignOutDeletesBothTailnetDirectories(t *testing.T) {
	state, run := t.TempDir(), t.TempDir()
	n := &Node{StateDir: state, RunDir: run}
	for _, d := range []string{n.tsnetDir(), n.tsnetRunDir()} {
		if err := os.MkdirAll(d, 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(d, "f"), []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	if err := n.stop(true); err != nil {
		t.Fatal(err)
	}
	for _, d := range []string{n.tsnetDir(), n.tsnetRunDir()} {
		if _, err := os.Stat(d); !os.IsNotExist(err) {
			t.Errorf("%s survived sign-out: %v", d, err)
		}
	}
}
