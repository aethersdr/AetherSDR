package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestFirstInstallLocalIdentitySurvivesReloadWithoutTailnet(t *testing.T) {
	state := t.TempDir()
	token := "private-fixture-admin-token"
	hash := sha256.Sum256([]byte(token))
	cfg := provisionConfig{TokenSHA256: hex.EncodeToString(hash[:]), Hostname: "aetherpd-fixture",
		LocalOnly: true, InstallationID: strings.Repeat("ab", 32)}
	data, err := json.Marshal(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(filepath.Join(state, "provision.json"), data, 0600); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 2; i++ {
		n := &Node{StateDir: state}
		if err = n.load(); err != nil {
			t.Fatal(err)
		}
		n.startFn = func(context.Context, string, time.Duration) error {
			t.Error("local install attempted tailnet join")
			return nil
		}
		n.Boot()
		status := n.Status()
		if status.State != stateLocal || status.Claimable || !status.Provisioned ||
			status.InstallationID != cfg.InstallationID || status.TailnetIP != "" || status.DNSName != "" {
			t.Fatalf("unexpected local identity: %+v", status)
		}
		if !n.checkToken(token) || n.checkToken("wrong-token") {
			t.Fatal("local credential boundary failed")
		}
		if n.bootDone != nil || n.srv != nil {
			t.Fatal("local installation started a tailnet worker")
		}
	}
	if _, err = os.Stat(filepath.Join(state, "tsnet")); !os.IsNotExist(err) {
		t.Fatalf("unexpected tailnet state: %v", err)
	}
}

func TestFirstInstallCorruptLocalIdentityRefused(t *testing.T) {
	for _, id := range []string{"", "ab", strings.Repeat("0", 64), strings.Repeat("AB", 32)} {
		t.Run(id, func(t *testing.T) {
			n := &Node{StateDir: t.TempDir()}
			data, _ := json.Marshal(provisionConfig{TokenSHA256: strings.Repeat("12", 32),
				Hostname: "aetherpd-fixture", LocalOnly: true, InstallationID: id})
			if err := os.WriteFile(n.cfgPath(), data, 0600); err != nil {
				t.Fatal(err)
			}
			if err := n.load(); err == nil {
				t.Fatal("accepted invalid local identity")
			}
		})
	}
}
