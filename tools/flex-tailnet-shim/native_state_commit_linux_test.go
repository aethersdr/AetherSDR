//go:build linux

package main

import (
	"bytes"
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"syscall"
	"testing"
	"time"

	"tailscale.com/ipn"
)

// This helper runs only in an owned test process. SIGKILL bypasses deferred
// cleanup so reopening exercises the selected file and actual crash remnant.
func TestNativeIdentityCrashHelper(t *testing.T) {
	if os.Getenv("AETHER_PD_IDENTITY_CRASH_HELPER") != "1" {
		t.Skip("subprocess helper")
	}
	state, err := newNativeStateStore(os.Getenv("AETHER_PD_IDENTITY_CRASH_PATH"))
	if err != nil {
		t.Fatal(err)
	}
	point := os.Getenv("AETHER_PD_IDENTITY_CRASH_POINT")
	crash := func(at string) {
		if at == point {
			if err := syscall.Kill(os.Getpid(), syscall.SIGKILL); err != nil {
				t.Fatal(err)
			}
			select {}
		}
	}
	parents := 0
	ops := nativeStateCommitOperations()
	ops.sync = func(file *os.File) error {
		info, err := file.Stat()
		if err != nil {
			return err
		}
		if err := file.Sync(); err != nil {
			return err
		}
		if info.IsDir() {
			parents++
			if parents == 1 {
				crash("parent-before")
			} else {
				crash("parent-after")
			}
		} else {
			crash("file-sync")
		}
		return nil
	}
	ops.rename = func(parent *os.File, from, to string) error {
		if err := renameNativeStateFileAt(parent, from, to); err != nil {
			return err
		}
		crash("rename")
		return nil
	}
	state.commitOps = &ops
	if err := state.WriteState("_machinekey", []byte("replacement")); err != nil {
		t.Fatal(err)
	}
	t.Fatal("identity write did not reach the requested crash boundary")
}

func TestNativeIdentityProcessDeathKeepsCompleteSelection(t *testing.T) {
	for _, point := range []string{"parent-before", "file-sync", "rename", "parent-after"} {
		t.Run(point, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "tailscaled.state")
			state, err := newNativeStateStore(path)
			if err != nil {
				t.Fatal(err)
			}
			if err := state.WriteState("_machinekey", []byte("previous")); err != nil {
				t.Fatal(err)
			}
			before, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			cmd := exec.CommandContext(ctx, os.Args[0], "-test.run=^TestNativeIdentityCrashHelper$")
			cmd.Env = append(os.Environ(), "AETHER_PD_IDENTITY_CRASH_HELPER=1",
				"AETHER_PD_IDENTITY_CRASH_PATH="+path, "AETHER_PD_IDENTITY_CRASH_POINT="+point)
			output, err := cmd.CombinedOutput()
			var exit *exec.ExitError
			if ctx.Err() != nil || !errors.As(err, &exit) {
				t.Fatalf("helper did not reach process death: %v, %s", err, output)
			}
			status, ok := exit.Sys().(syscall.WaitStatus)
			if !ok || !status.Signaled() || status.Signal() != syscall.SIGKILL {
				t.Fatalf("helper failed without the requested SIGKILL: %v, %s", err, output)
			}
			want := "previous"
			if point == "rename" || point == "parent-after" {
				want = "replacement"
			}
			// Open the exact file without a tsnet client or network activity.
			reopened, err := newNativeStateStore(path)
			if err != nil {
				t.Fatal("crash made the selected identity unreadable", err)
			}
			value, err := reopened.ReadState("_machinekey")
			if err != nil || string(value) != want {
				t.Fatal("restart selected a different identity", err)
			}
			if want == "previous" {
				actual, err := os.ReadFile(path)
				if err != nil || !bytes.Equal(before, actual) {
					t.Fatal("pre-rename crash changed the committed file", err)
				}
			}
			if point == "file-sync" {
				remnant, err := newNativeStateStore(path + ".tmp")
				if err != nil {
					t.Fatal("complete crash remnant lost", err)
				}
				value, err := remnant.ReadState("_machinekey")
				if err != nil || string(value) != "replacement" {
					t.Fatal("crash remnant differs from the pending identity", err)
				}
			} else if _, err := os.Lstat(path + ".tmp"); !os.IsNotExist(err) {
				t.Fatal("unexpected temporary identity after process death", err)
			}
			if err := reopened.WriteState("_machinekey", []byte("explicit-retry")); err != nil {
				t.Fatal("explicit retry could not replace the complete selection", err)
			}
			after, err := newNativeStateStore(path)
			if err != nil {
				t.Fatal(err)
			}
			value, err = after.ReadState("_machinekey")
			if err != nil || string(value) != "explicit-retry" {
				t.Fatal("explicit retry did not persist", err)
			}
			if _, err := os.Lstat(path + ".tmp"); !os.IsNotExist(err) {
				t.Fatal("retry left a crash remnant", err)
			}
		})
	}
}

func recordingNativeCommitOps(t *testing.T, failure string) (nativeStateCommitOps, *[]string) {
	t.Helper()
	events := []string{}
	directories := 0
	record := func(event string) error {
		events = append(events, event)
		if event == failure {
			return os.ErrPermission
		}
		return nil
	}
	return nativeStateCommitOps{
		sync: func(file *os.File) error {
			info, err := file.Stat()
			if err != nil {
				return err
			}
			event := "file-sync"
			if info.IsDir() {
				directories++
				event = "parent-before"
				if directories == 2 {
					event = "parent-after"
				}
			}
			if err := record(event); err != nil {
				return err
			}
			return file.Sync()
		},
		rename: func(parent *os.File, from, to string) error {
			if err := record("rename"); err != nil {
				return err
			}
			return renameNativeStateFileAt(parent, from, to)
		},
	}, &events
}

func TestNativeCommitWriteOrderAndFailureBoundary(t *testing.T) {
	order := []string{"parent-before", "file-sync", "rename", "parent-after"}
	for _, failure := range []string{"", "parent-before", "file-sync", "rename", "parent-after"} {
		t.Run("failure="+failure, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "identity")
			if err := writeNativeStateFile(path, []byte("previous")); err != nil {
				t.Fatal(err)
			}
			ops, events := recordingNativeCommitOps(t, failure)
			err := writeNativeStateFileWithOps(path, []byte("replacement"), ops)
			if (err == nil) != (failure == "") {
				t.Fatalf("failure %q returned %v", failure, err)
			}
			if errors.Is(err, errNativeCommitUncertain) != (failure == "parent-after") {
				t.Fatal("pre-rename failure and uncertain post-rename commit conflated", err)
			}
			if failure != "" && !errors.Is(err, os.ErrPermission) {
				t.Fatal("underlying failure lost", err)
			}
			want := order
			for i, event := range order {
				if event == failure {
					want = order[:i+1]
				}
			}
			if !reflect.DeepEqual(*events, want) {
				t.Fatalf("commit order %v, want %v", *events, want)
			}
			selected, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			wantBytes := "previous"
			if failure == "" || failure == "parent-after" {
				wantBytes = "replacement"
			}
			if string(selected) != wantBytes {
				t.Fatalf("selected identity %q, want %q", selected, wantBytes)
			}
			if _, err := os.Lstat(path + ".tmp"); !os.IsNotExist(err) {
				t.Fatal("write left a temporary sibling", err)
			}
		})
	}
}

func TestNativeCommitUncertainStateCannotSkipRepair(t *testing.T) {
	for _, retry := range []string{"previous", "replacement", "absent-delete"} {
		t.Run(retry, func(t *testing.T) {
			_, store := nativeTestStore(t)
			state := store.(*nativeStateStore)
			if err := state.WriteState("key", []byte("previous")); err != nil {
				t.Fatal(err)
			}
			ops, _ := recordingNativeCommitOps(t, "parent-after")
			state.commitOps = &ops
			if err := state.WriteState("key", []byte("replacement")); !errors.Is(err, errNativeCommitUncertain) {
				t.Fatal("uncertain replacement reported committed", err)
			}
			if _, err := state.ReadState("key"); !errors.Is(err, errNativeCommitUncertain) {
				t.Fatal("uncertain state exposed as a usable identity", err)
			}
			// Export is a snapshot of the last successful commit, not live access.
			for key, data := range state.All() {
				if key != "key" || string(data) != "previous" {
					t.Fatal("failed write published a new export snapshot")
				}
			}
			ops, events := recordingNativeCommitOps(t, "")
			state.commitOps = &ops
			key, value := ipn.StateKey("key"), []byte(retry)
			if retry == "absent-delete" {
				key, value = "absent", nil
			}
			if err := state.WriteState(key, value); err != nil {
				t.Fatal(err)
			}
			if len(*events) != 4 {
				t.Fatal("cache equality skipped the required durable repair", *events)
			}
			expected := retry
			if retry == "absent-delete" {
				expected = "previous"
			}
			data, err := state.ReadState("key")
			if err != nil || string(data) != expected {
				t.Fatal("repaired cache differs", err)
			}
			reopened, err := newNativeStateStore(state.path)
			if err != nil {
				t.Fatal(err)
			}
			data, err = reopened.ReadState("key")
			if err != nil || string(data) != expected {
				t.Fatal("successful retry did not repair the selected file", err)
			}
		})
	}
}

func TestNativeCommitUncertainConfigRevokesPDAndAdmission(t *testing.T) {
	for _, failure := range []string{"file-sync", "parent-after"} {
		t.Run(failure, func(t *testing.T) {
			n, process, arm := pdAuthorityFixture(t)
			n.RunDir = t.TempDir()
			if err := n.save(n.cfg); err != nil {
				t.Fatal(err)
			}
			before, err := os.ReadFile(n.cfgPath())
			if err != nil {
				t.Fatal(err)
			}
			ops, _ := recordingNativeCommitOps(t, failure)
			n.configWrite = func(data []byte) error { return writeNativeStateFileWithOps(n.cfgPath(), data, ops) }
			boot, cancel := context.WithCancel(context.Background())
			defer cancel()
			n.bootCancel = cancel
			pending, cancelPending := context.WithCancel(context.Background())
			defer cancelPending()
			n.pdStarting = &pdSessionStart{cancel: cancelPending}
			err = n.changeConfig(func(cfg *provisionConfig) {
				cfg.Hostname = "replacement-name"
				cfg.TokenSHA256 = sha256Hex("replacement-admin")
			})
			if err == nil {
				t.Fatal("unconfirmed configuration accepted")
			}
			if failure == "file-sync" {
				assertPDAuthorityUnchanged(t, process, arm)
				if !n.checkToken("current-admin") || boot.Err() != nil || pending.Err() != nil {
					t.Fatal("pre-rename refusal changed live authority")
				}
				after, err := os.ReadFile(n.cfgPath())
				if err != nil || !bytes.Equal(before, after) {
					t.Fatal("pre-rename failure replaced the saved owner", err)
				}
				return
			}
			assertPDAuthorityRevoked(t, process, arm)
			if n.checkToken("current-admin") || n.checkToken("replacement-admin") || n.Status().Claimable || boot.Err() != context.Canceled || pending.Err() != context.Canceled {
				t.Fatal("uncertain owner configuration retained control or enrollment")
			}
			if n.Status().State != stateError || n.Status().LastError == "" {
				t.Fatal("uncertain commit was not surfaced")
			}
			n.Boot()
			if n.bootDone != nil {
				t.Fatal("uncertain configuration restarted enrollment")
			}
			n.configWrite = func([]byte) error { t.Fatal("latched configuration was written again"); return nil }
			if err := n.changeConfig(func(cfg *provisionConfig) { cfg.Hostname = "unapproved-retry" }); !errors.Is(err, errNativeStateUnavailable) {
				t.Fatal("uncertain configuration accepted another writer", err)
			}
			loaded := credentialReload(t, n)
			if loaded.cfg.Hostname != "replacement-name" || !loaded.checkToken("replacement-admin") || loaded.checkToken("current-admin") {
				t.Fatal("explicit fresh load did not retain the selected owner")
			}
		})
	}
}

func TestNativeCommitUncertainFirstClaimDoesNotReopenEnrollment(t *testing.T) {
	n := &Node{StateDir: t.TempDir(), RunDir: t.TempDir()}
	if err := n.load(); err != nil || !n.Status().Claimable {
		t.Fatal("invalid first-use fixture", err)
	}
	ops, _ := recordingNativeCommitOps(t, "parent-after")
	writes := 0
	n.configWrite = func(data []byte) error {
		writes++
		return writeNativeStateFileWithOps(n.cfgPath(), data, ops)
	}
	n.startFn = func(context.Context, string, time.Duration) error {
		t.Fatal("unconfirmed first claim started enrollment")
		return nil
	}
	if token, err := n.Provision("", "unused-fixture-key", "radio", nil, nil, nil); token != "" || !errors.Is(err, errNativeCommitUncertain) {
		t.Fatal("unconfirmed first claim reported a usable token", err)
	}
	if n.Status().Claimable {
		t.Fatal("uncertain first claim reopened token-free enrollment")
	}
	if _, err := n.Provision("", "unused-fixture-key", "replacement", nil, nil, nil); err == nil || writes != 1 {
		t.Fatal("a second first-use attempt replaced the uncertain identity", err)
	}
	if data, err := os.ReadFile(n.cfgPath()); err != nil || len(data) == 0 {
		t.Fatal("uncertain identity was erased", err)
	}
}

func TestNativeCommitPinsDirectoryThroughReplacementAndCleanup(t *testing.T) {
	for _, failFileSync := range []bool{false, true} {
		t.Run(map[bool]string{false: "replacement", true: "cleanup"}[failFileSync], func(t *testing.T) {
			root := t.TempDir()
			directory := filepath.Join(root, "state")
			moved := filepath.Join(root, "moved")
			if err := os.Mkdir(directory, 0700); err != nil {
				t.Fatal(err)
			}
			path := filepath.Join(directory, "identity")
			if err := writeNativeStateFile(path, []byte("previous")); err != nil {
				t.Fatal(err)
			}
			ops := nativeStateCommitOperations()
			ops.sync = func(file *os.File) error {
				info, err := file.Stat()
				if err != nil {
					return err
				}
				if !info.IsDir() {
					if err := os.Rename(directory, moved); err != nil {
						t.Fatal(err)
					}
					if err := os.Mkdir(directory, 0700); err != nil {
						t.Fatal(err)
					}
					for _, name := range []string{"identity", "identity.tmp"} {
						if err := os.WriteFile(filepath.Join(directory, name), []byte("foreign"), 0600); err != nil {
							t.Fatal(err)
						}
					}
					if failFileSync {
						return os.ErrPermission
					}
				}
				return file.Sync()
			}
			err := writeNativeStateFileWithOps(path, []byte("replacement"), ops)
			if err == nil || (!failFileSync && !errors.Is(err, errNativeCommitUncertain)) {
				t.Fatal("changed parent reported successful selection", err)
			}
			for _, name := range []string{"identity", "identity.tmp"} {
				data, err := os.ReadFile(filepath.Join(directory, name))
				if err != nil || string(data) != "foreign" {
					t.Fatal("write or cleanup touched the replacement parent", name, err)
				}
			}
			if _, err := os.Stat(filepath.Join(moved, "identity.tmp")); !os.IsNotExist(err) {
				t.Fatal("pinned temporary sibling was not retired", err)
			}
		})
	}
}

func TestNativeCommitLoadMigrationCannotClearUncertainLatch(t *testing.T) {
	n := credentialFixture(t, true)
	n.cfg.Allow = []string{"legacy-fixture"}
	if err := n.save(n.cfg); err != nil {
		t.Fatal(err)
	}
	loaded := &Node{StateDir: n.StateDir, RunDir: n.RunDir}
	ops, _ := recordingNativeCommitOps(t, "parent-after")
	loaded.configWrite = func(data []byte) error { return writeNativeStateFileWithOps(loaded.cfgPath(), data, ops) }
	if err := loaded.load(); !errors.Is(err, errNativeCommitUncertain) {
		t.Fatal("startup migration ignored unconfirmed commit", err)
	}
	if loaded.Status().State != stateError || loaded.Status().Claimable || loaded.checkToken("old-owner-fixture") {
		t.Fatal("load completion cleared the uncertain-commit refusal")
	}
}

func TestNativeCommitRefusesLinkedParent(t *testing.T) {
	root := t.TempDir()
	actual := filepath.Join(root, "actual")
	if err := os.Mkdir(actual, 0700); err != nil {
		t.Fatal(err)
	}
	alias := filepath.Join(root, "alias")
	if err := os.Symlink(actual, alias); err != nil {
		t.Fatal(err)
	}
	if err := writeNativeStateFile(filepath.Join(alias, "identity"), []byte("new")); err == nil {
		t.Fatal("linked native identity parent admitted")
	}
	if _, err := os.Stat(filepath.Join(actual, "identity")); !os.IsNotExist(err) {
		t.Fatal("parent refusal mutated the target", err)
	}
}
