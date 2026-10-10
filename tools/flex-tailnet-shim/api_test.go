package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func newTestNode(t *testing.T, token string) *Node {
	t.Helper()
	n := &Node{StateDir: t.TempDir(), RadioAddr: "127.0.0.1"}
	if token != "" {
		n.cfg = provisionConfig{TokenSHA256: sha256Hex(token), Hostname: "flex-test"}
		if err := n.save(n.cfg); err != nil {
			t.Fatal(err)
		}
	}
	if err := n.load(); err != nil {
		t.Fatal(err)
	}
	return n
}

func call(t *testing.T, h http.Handler, method, path, remote, token, body string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(method, path, strings.NewReader(body))
	req.RemoteAddr = remote
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	return rec
}

func TestAPIRefusesNonLANCallers(t *testing.T) {
	h := NewAPI(newTestNode(t, ""))
	for _, remote := range []string{"172.30.1.5:4000", "8.8.8.8:4000", "100.86.176.80:4000"} {
		if rec := call(t, h, "GET", "/v1/status", remote, "", ""); rec.Code != http.StatusForbidden {
			t.Errorf("%s: got %d, want 403", remote, rec.Code)
		}
	}
	if rec := call(t, h, "GET", "/v1/status", "192.168.50.236:4000", "", ""); rec.Code != http.StatusOK {
		t.Fatalf("LAN caller refused: %d", rec.Code)
	}
}

func TestStatusUnprovisionedHasNoSecrets(t *testing.T) {
	h := NewAPI(newTestNode(t, ""))
	rec := call(t, h, "GET", "/v1/status", "192.168.1.10:5000", "", "")
	var st Status
	if err := json.Unmarshal(rec.Body.Bytes(), &st); err != nil {
		t.Fatal(err)
	}
	if st.Provisioned || st.State != stateUnprovisioned || st.Version != shimVersion {
		t.Fatalf("unexpected status %+v", st)
	}
	if strings.Contains(rec.Body.String(), "token") || strings.Contains(rec.Body.String(), "tskey") {
		t.Fatalf("status leaks a secret: %s", rec.Body.String())
	}
}

func TestProvisionValidatesInput(t *testing.T) {
	h := NewAPI(newTestNode(t, ""))
	for _, body := range []string{
		`{"auth_key":"not-a-key"}`,
		`{"auth_key":"tskey-auth-abcdefgh12345","hostname":"Bad Host"}`,
		`{"auth_key":"tskey-auth-abcdefgh12345","allow":["; rm -rf /"]}`,
		`{"auth_key":"tskey-auth-abcdefgh12345","extra":1}`,
		`garbage`,
	} {
		if rec := call(t, h, "POST", "/v1/provision", "192.168.1.10:5000", "", body); rec.Code != http.StatusBadRequest {
			t.Errorf("%s: got %d, want 400", body, rec.Code)
		}
	}
}

func TestProvisionedNodeRequiresToken(t *testing.T) {
	const token = "test-admin-token"
	n := newTestNode(t, token)
	h := NewAPI(n)
	key := `{"auth_key":"tskey-auth-abcdefgh12345"}`
	if rec := call(t, h, "POST", "/v1/provision", "192.168.1.10:5000", "", key); rec.Code != http.StatusUnauthorized {
		t.Fatalf("re-provision without token: got %d, want 401", rec.Code)
	}
	if rec := call(t, h, "POST", "/v1/provision", "192.168.1.10:5000", "wrong", key); rec.Code != http.StatusUnauthorized {
		t.Fatalf("re-provision with wrong token: got %d, want 401", rec.Code)
	}
	if rec := call(t, h, "PUT", "/v1/allow", "192.168.1.10:5000", "", `{"allow":["tag:ops"]}`); rec.Code != http.StatusUnauthorized {
		t.Fatalf("allow without token: got %d", rec.Code)
	}
	rec := call(t, h, "PUT", "/v1/allow", "192.168.1.10:5000", token, `{"allow":["tag:ops","kk7gwy@example.com"]}`)
	if rec.Code != http.StatusOK {
		t.Fatalf("allow with token: got %d %s", rec.Code, rec.Body.String())
	}
	if got := n.Status().Allow; len(got) != 2 || got[0] != "tag:ops" {
		t.Fatalf("allowlist not applied: %v", got)
	}
	if rec := call(t, h, "POST", "/v1/signout", "192.168.1.10:5000", "", ""); rec.Code != http.StatusUnauthorized {
		t.Fatalf("signout without token: got %d", rec.Code)
	}
	if rec := call(t, h, "POST", "/v1/signout", "192.168.1.10:5000", token, ""); rec.Code != http.StatusOK {
		t.Fatalf("signout with token: got %d", rec.Code)
	}
	if st := n.Status(); st.Provisioned || st.State != stateUnprovisioned {
		t.Fatalf("not unprovisioned after sign-out: %+v", st)
	}
	if _, err := os.Stat(filepath.Join(n.StateDir, "provision.json")); !os.IsNotExist(err) {
		t.Fatalf("provision.json survived sign-out: %v", err)
	}
	// After sign-out the old token is worthless.
	if n.checkToken(token) {
		t.Fatal("old admin token still accepted")
	}
}

func TestSavedConfigHoldsOnlyTokenHash(t *testing.T) {
	n := newTestNode(t, "plaintext-admin-token")
	b, err := os.ReadFile(filepath.Join(n.StateDir, "provision.json"))
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(b), "plaintext-admin-token") {
		t.Fatal("admin token stored in plain text")
	}
}
