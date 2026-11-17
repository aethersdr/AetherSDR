package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"tailscale.com/ipn"
	"tailscale.com/ipn/store"
)

const testNativeFileLimit = 64 * 1024 // Matches the authenticated installer's state-transfer limit.

func nativeTestStore(t *testing.T) (*Node, ipn.StateStore) {
	t.Helper()
	n := &Node{StateDir: t.TempDir(), RunDir: t.TempDir()}
	srv, err := n.newTsnetServer("radio", "")
	if err != nil {
		t.Fatal(err)
	}
	return n, srv.Store
}

func TestNativeStateRejectsOversizeWithoutReplacingIdentity(t *testing.T) {
	n, st := nativeTestStore(t)
	key := ipn.StateKey("_machinekey")
	if err := st.WriteState(key, []byte("previous")); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(n.tsnetDir(), "tailscaled.state")
	before, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	for _, tc := range []struct {
		name string
		key  ipn.StateKey
		data []byte
	}{
		{"value", key, bytes.Repeat([]byte("x"), testNativeFileLimit)},
		{"key", ipn.StateKey(strings.Repeat("k", testNativeFileLimit)), []byte("x")},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if err := st.WriteState(tc.key, tc.data); err == nil {
				t.Fatal("oversize identity write accepted")
			}
			actual, err := st.ReadState(key)
			if err != nil || string(actual) != "previous" {
				t.Fatalf("identity changed after refusal: %v", err)
			}
			after, err := os.ReadFile(path)
			if err != nil || !bytes.Equal(before, after) {
				t.Fatalf("persistent identity changed after refusal: %v", err)
			}
		})
	}
}

func TestNativeStateFailedWriteDoesNotPoisonCacheOrRetry(t *testing.T) {
	n, st := nativeTestStore(t)
	key := ipn.StateKey("_machinekey")
	if err := st.WriteState(key, []byte("previous")); err != nil {
		t.Fatal(err)
	}
	away := n.tsnetDir() + "-saved"
	if err := os.Rename(n.tsnetDir(), away); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(n.tsnetDir(), []byte("block parent traversal"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := st.WriteState(key, []byte("replacement")); err == nil {
		t.Fatal("write through a non-directory parent accepted")
	}
	value, err := st.ReadState(key)
	if err != nil || string(value) != "previous" {
		t.Error("failed write replaced the in-memory identity")
	}
	if err := os.Remove(n.tsnetDir()); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(away, n.tsnetDir()); err != nil {
		t.Fatal(err)
	}
	if err := st.WriteState(key, []byte("replacement")); err != nil {
		t.Fatal(err)
	}
	reopened, err := store.NewFileStore(t.Logf, filepath.Join(n.tsnetDir(), "tailscaled.state"))
	if err != nil {
		t.Fatal(err)
	}
	value, err = reopened.ReadState(key)
	if err != nil || string(value) != "replacement" {
		t.Fatal("retry reported success without persisting replacement identity")
	}
}

func TestNativeStateReadAndExportCannotMutateIdentity(t *testing.T) {
	_, st := nativeTestStore(t)
	key := ipn.StateKey("key")
	input := []byte("identity")
	if err := st.WriteState(key, input); err != nil {
		t.Fatal(err)
	}
	input[0] = 'X'
	value, err := st.ReadState(key)
	if err != nil {
		t.Fatal(err)
	}
	value[0] = 'Y'
	if exported, ok := st.(store.ExportableStore); ok {
		for _, data := range exported.All() {
			data[0] = 'Z'
		}
	} else {
		t.Fatal("state store cannot export its identity")
	}
	value, err = st.ReadState(key)
	if err != nil || string(value) != "identity" {
		t.Fatal("caller changed state without an admitted persistent write")
	}
}

func TestNativeStateExactEncodedBoundaryAndAggregate(t *testing.T) {
	n, st := nativeTestStore(t)
	// Include base64 expansion and JSON keys in the exact serialized boundary.
	key := ipn.StateKey("boundary")
	data := bytes.Repeat([]byte("z"), 3*((testNativeFileLimit-32)/4))
	encoded, _ := json.MarshalIndent(map[ipn.StateKey][]byte{key: data}, "", "  ")
	key += ipn.StateKey(strings.Repeat("k", testNativeFileLimit-len(encoded)))
	encoded, _ = json.MarshalIndent(map[ipn.StateKey][]byte{key: data}, "", "  ")
	if len(encoded) != testNativeFileLimit {
		t.Fatal("invalid exact-limit fixture")
	}
	if err := st.WriteState(key, data); err != nil {
		t.Fatalf("exact boundary refused: %v", err)
	}
	saved, err := os.ReadFile(filepath.Join(n.tsnetDir(), "tailscaled.state"))
	if err != nil || len(saved) != testNativeFileLimit {
		t.Fatal("exact encoded size differs")
	}
	if err := st.WriteState("extra", []byte("x")); err == nil {
		t.Fatal("aggregate size was not bounded")
	}
	if err := st.WriteState(key, nil); err != nil {
		t.Fatal(err)
	}
	if _, err := st.ReadState(key); !errors.Is(err, ipn.ErrStateNotExist) {
		t.Fatal("deletion did not remove key")
	}
	if err := st.WriteState("extra", []byte{}); err != nil {
		t.Fatal(err)
	}
	if v, err := st.ReadState("extra"); err != nil || v == nil || len(v) != 0 {
		t.Fatal("empty value confused with deletion")
	}
}

func TestNativeStateLoadBoundAndUpstreamCompatibility(t *testing.T) {
	n, st := nativeTestStore(t)
	key := ipn.StateKey("_machinekey")
	if err := st.WriteState(key, []byte("identity")); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(n.tsnetDir(), "tailscaled.state")
	original, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	for _, payload := range [][]byte{bytes.Repeat([]byte(" "), testNativeFileLimit+1), []byte("{broken"), []byte("null")} {
		if err := os.WriteFile(path, payload, 0600); err != nil {
			t.Fatal(err)
		}
		if _, err := n.newTsnetServer("radio", ""); err == nil {
			t.Error("invalid or oversized state loaded")
		}
		actual, err := os.ReadFile(path)
		if err != nil || !bytes.Equal(payload, actual) {
			t.Fatal("refused identity was modified")
		}
	}
	if err := os.WriteFile(path, original, 0600); err != nil {
		t.Fatal(err)
	}
	legacy, err := store.NewFileStore(t.Logf, path)
	if err != nil {
		t.Fatal(err)
	}
	if v, err := legacy.ReadState(key); err != nil || string(v) != "identity" {
		t.Fatal("upstream cannot read native identity")
	}
	if err := legacy.WriteState("_current-profile", []byte("profile")); err != nil {
		t.Fatal(err)
	}
	srv, err := n.newTsnetServer("radio", "")
	if err != nil {
		t.Fatal(err)
	}
	if v, err := srv.Store.ReadState("_current-profile"); err != nil || string(v) != "profile" {
		t.Fatal("native cannot read upstream identity")
	}
}

func TestNativeStateConcurrentUpdatesSurviveReopen(t *testing.T) {
	n, st := nativeTestStore(t)
	var wg sync.WaitGroup
	for i := range 12 {
		wg.Go(func() {
			key := ipn.StateKey(strings.Repeat("k", i+1))
			for range 4 {
				if err := st.WriteState(key, []byte("identity")); err != nil {
					t.Error(err)
				}
				if _, err := st.ReadState(key); err != nil {
					t.Error(err)
				}
			}
		})
	}
	wg.Wait()
	srv, err := n.newTsnetServer("radio", "")
	if err != nil {
		t.Fatal(err)
	}
	for i := range 12 {
		if v, err := srv.Store.ReadState(ipn.StateKey(strings.Repeat("k", i+1))); err != nil || string(v) != "identity" {
			t.Fatal("concurrent identity lost after reopen")
		}
	}
}

func TestNativeProvisionFileBoundPreservesPrevious(t *testing.T) {
	n := &Node{StateDir: t.TempDir(), RunDir: t.TempDir()}
	cfg := provisionConfig{TokenSHA256: strings.Repeat("a", 64), Hostname: "radio"}
	if err := n.save(cfg); err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadFile(n.cfgPath())
	if err != nil {
		t.Fatal(err)
	}
	cfg.Hostname = strings.Repeat("x", testNativeFileLimit)
	if err := n.save(cfg); err == nil {
		t.Error("oversized native provision config persisted")
	}
	after, err := os.ReadFile(n.cfgPath())
	if err != nil || !bytes.Equal(before, after) {
		t.Error("oversize save replaced the prior provision config")
	}
	payload, _ := json.Marshal(cfg)
	if err := os.WriteFile(n.cfgPath(), payload, 0600); err != nil {
		t.Fatal(err)
	}
	if err := n.load(); err == nil {
		t.Fatal("oversize config loaded")
	}
}

func TestNativeStateReusesOneInterruptedTemporary(t *testing.T) {
	n, st := nativeTestStore(t)
	path := filepath.Join(n.tsnetDir(), "tailscaled.state")
	if err := os.WriteFile(path+".tmp", bytes.Repeat([]byte("x"), testNativeFileLimit), 0600); err != nil {
		t.Fatal(err)
	}
	for range 4 {
		if err := st.WriteState("key", []byte("new identity")); err != nil {
			t.Fatal(err)
		}
	}
	entries, err := os.ReadDir(n.tsnetDir())
	if err != nil || len(entries) != 1 || entries[0].Name() != "tailscaled.state" {
		t.Fatal("temporary identity accumulated after retry")
	}
}

func TestNativeStateRenameFailureKeepsCacheAndRemovesTemporary(t *testing.T) {
	n, st := nativeTestStore(t)
	path := filepath.Join(n.tsnetDir(), "tailscaled.state")
	key := ipn.StateKey("key")
	if err := st.WriteState(key, []byte("previous")); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(path, path+"-saved"); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(path, 0700); err != nil {
		t.Fatal(err)
	}
	if err := st.WriteState(key, []byte("replacement")); err == nil {
		t.Fatal("rename onto directory accepted")
	}
	if _, err := os.Lstat(path + ".tmp"); !errors.Is(err, os.ErrNotExist) {
		t.Fatal("failed write left a temporary identity")
	}
	if value, err := st.ReadState(key); err != nil || string(value) != "previous" {
		t.Fatal("rename failure replaced cached identity")
	}
}

func TestNativeProvisionBudgetDoesNotChangeContainerLayout(t *testing.T) {
	n := &Node{StateDir: t.TempDir()}
	cfg := provisionConfig{TokenSHA256: strings.Repeat("a", 64), Hostname: strings.Repeat("x", testNativeFileLimit)}
	if err := n.save(cfg); err != nil {
		t.Fatal(err)
	}
	if err := n.load(); err != nil {
		t.Fatal(err)
	}
	if n.cfg.Hostname != cfg.Hostname {
		t.Fatal("container config changed")
	}
}

func TestNativeJoinedBeforeOversizeRemainsOwned(t *testing.T) {
	dir := t.TempDir()
	payload := []byte(`{"_current-profile":"","padding":"` + strings.Repeat("x", testNativeFileLimit) + `"}`)
	if err := os.WriteFile(filepath.Join(dir, "tailscaled.state"), payload, 0600); err != nil {
		t.Fatal(err)
	}
	if !joinedBefore(dir, true) {
		t.Fatal("oversized native identity became claimable")
	}
	if joinedBefore(dir, false) {
		t.Fatal("container-layout sign-in rule changed")
	}
}

func TestNativeStateLoadFailureCannotBecomeFirstUse(t *testing.T) {
	for _, payload := range [][]byte{[]byte(`{"token_sha256":"` + strings.Repeat("a", 64) + `","hostname":"` + strings.Repeat("x", testNativeFileLimit) + `"}`), []byte(`{"local_only":true,"token_sha256":"` + sha256Hex("fixture-owner") + `","hostname":"radio","installation_id":"invalid"}`), []byte("{broken")} {
		t.Run(string(payload[:4]), func(t *testing.T) {
			n := &Node{StateDir: t.TempDir(), RunDir: t.TempDir()}
			if err := os.WriteFile(n.cfgPath(), payload, 0600); err != nil {
				t.Fatal(err)
			}
			if err := n.load(); err == nil {
				t.Fatal("invalid native identity loaded")
			}
			n.Boot()
			status := n.Status()
			if status.Claimable || status.State != stateError {
				t.Errorf("failed identity load exposed first-use enrollment: state=%q claimable=%t", status.State, status.Claimable)
			}
			if n.checkToken("fixture-owner") {
				t.Error("partial failed identity still authorizes control")
			}
			if end, err := n.beginOp("", true); err == nil {
				end()
				t.Error("failed identity load admitted unauthenticated enrollment")
			}
			if n.bootDone != nil {
				t.Error("failed native identity started a boot worker")
			}
			saved, err := os.ReadFile(n.cfgPath())
			if err != nil || !bytes.Equal(saved, payload) {
				t.Fatal("failed load modified saved identity")
			}
		})
	}
}

func TestNativeProvisionConcurrentSavesStayAtomic(t *testing.T) {
	n := &Node{StateDir: t.TempDir(), RunDir: t.TempDir()}
	var wg sync.WaitGroup
	for i := range 12 {
		wg.Go(func() {
			cfg := provisionConfig{TokenSHA256: strings.Repeat("a", 64), Hostname: strings.Repeat(string(rune('a'+i)), 32)}
			for range 4 {
				if err := n.save(cfg); err != nil {
					t.Error(err)
				}
			}
		})
	}
	wg.Wait()
	data, err := os.ReadFile(n.cfgPath())
	if err != nil {
		t.Fatal(err)
	}
	var cfg provisionConfig
	if err := json.Unmarshal(data, &cfg); err != nil {
		t.Fatal("concurrent save tore configuration")
	}
	if len(cfg.Hostname) != 32 || strings.Trim(cfg.Hostname, string(cfg.Hostname[0])) != "" {
		t.Fatal("concurrent save mixed configuration values")
	}
	if _, err := os.Lstat(n.cfgPath() + ".tmp"); !errors.Is(err, os.ErrNotExist) {
		t.Fatal("concurrent saves left a temporary configuration")
	}
}

func TestNativeStateErrorFormattingDoesNotExposeIdentity(t *testing.T) {
	_, st := nativeTestStore(t)
	key := ipn.StateKey("fixture-private-identity-key")
	secret := []byte("fixture-private-value")
	if err := st.WriteState(key, secret); err != nil {
		t.Fatal(err)
	}
	for _, format := range []string{"%v", "%+v", "%#v", "%s"} {
		text := fmt.Sprintf(format, st)
		if strings.Contains(text, string(key)) || strings.Contains(text, string(secret)) || strings.Contains(text, fmt.Sprint(secret)) {
			t.Fatal("store formatting exposes identity")
		}
	}
}
