package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
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

// installLauncher copies native/shim.sh into waveformDir with a stub
// unpacker whose "shim" only records SHIM_STATE_DIR in the returned file.
func installLauncher(t *testing.T, waveformDir string) string {
	t.Helper()
	launcher, err := os.ReadFile(filepath.Join("native", "shim.sh"))
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(waveformDir, "shim.sh"), launcher, 0o755); err != nil {
		t.Fatal(err)
	}
	report := filepath.Join(t.TempDir(), "state-dir")
	shim := "#!/bin/sh\necho \"$SHIM_STATE_DIR\" > '" + report + "'\n"
	unpack := "#!/bin/sh\ncat > \"$2\" <<'EOF'\n" + shim + "EOF\nchmod 755 \"$2\"\n"
	if err := os.WriteFile(filepath.Join(waveformDir, "unpack-armv7"), []byte(unpack), 0o755); err != nil {
		t.Fatal(err)
	}
	return report
}

// runLauncher starts the copied launcher as an ARMv7 radio would (uname is
// stubbed) and returns the state directory it gave the shim. Everything
// stays under the test's temporary directories.
func runLauncher(t *testing.T, waveformDir, runDir, report string) string {
	t.Helper()
	bin := t.TempDir()
	if err := os.WriteFile(filepath.Join(bin, "uname"), []byte("#!/bin/sh\necho armv7l\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command("/bin/sh", filepath.Join(waveformDir, "shim.sh"))
	cmd.Env = []string{"PATH=" + bin + ":/usr/bin:/bin", "SHIM_RUN_DIR=" + runDir}
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("launcher: %v\n%s", err, out)
	}
	got, err := os.ReadFile(report)
	if err != nil {
		t.Fatalf("the launcher never started the shim: %v", err)
	}
	return strings.TrimSpace(string(got))
}

// The saved sign-in lives beside the waveform, so uninstalling removes it;
// when that folder isn't writable it goes to RAM, never anywhere that
// outlives the waveform (AetherSDR's Reset depends on this).
func TestLauncherKeepsStateWithTheWaveform(t *testing.T) {
	if _, err := os.Stat("/bin/sh"); err != nil {
		t.Skip("no /bin/sh")
	}
	waveform, run := t.TempDir(), t.TempDir()
	report := installLauncher(t, waveform)
	if got, want := runLauncher(t, waveform, run, report), filepath.Join(waveform, "state"); got != want {
		t.Fatalf("writable waveform folder: state in %q, want %q", got, want)
	}

	if os.Geteuid() == 0 {
		t.Skip("root ignores the read-only folder")
	}
	readOnly, run2 := t.TempDir(), t.TempDir()
	report2 := installLauncher(t, readOnly)
	if err := os.Chmod(readOnly, 0o555); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.Chmod(readOnly, 0o755) })
	if got, want := runLauncher(t, readOnly, run2, report2), filepath.Join(run2, "state"); got != want {
		t.Fatalf("read-only waveform folder: state in %q, want RAM %q", got, want)
	}
}
