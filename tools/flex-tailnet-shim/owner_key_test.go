package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"os"
	"reflect"
	"strings"
	"sync/atomic"
	"testing"
)

const ownerKeyCandidate = "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef"

func ownerKeyFixtureRequest(n *Node) ownerKeyRequest {
	return ownerKeyRequest{InstallationID: n.cfg.InstallationID, NewToken: ownerKeyCandidate}
}

func ownerKeyBody(t *testing.T, req ownerKeyRequest) string {
	t.Helper()
	raw, err := json.Marshal(req)
	if err != nil {
		t.Fatal(err)
	}
	return string(raw)
}

func TestOwnerKeyRequestBoundary(t *testing.T) {
	n := managedIdentityFixture(t)
	valid := ownerKeyBody(t, ownerKeyFixtureRequest(n))
	for _, body := range []string{
		"", "null", "[]", "{}", valid + "{}", valid + " null",
		strings.Repeat(" ", 513), valid + strings.Repeat(" ", 513),
		strings.Replace(valid, `"new_token":`, `"new_token":null,"new_token":`, 1),
		strings.Replace(valid, "}", `,"command":"anything"}`, 1),
		strings.Replace(valid, ownerKeyCandidate, strings.ToUpper(ownerKeyCandidate), 1),
		strings.Replace(valid, ownerKeyCandidate, strings.Repeat("0", 64), 1),
		strings.Replace(valid, ownerKeyCandidate, ownerKeyCandidate[:63], 1),
		strings.Replace(valid, ownerKeyCandidate, "g"+ownerKeyCandidate[1:], 1),
		strings.Replace(valid, `"`+ownerKeyCandidate+`"`, "null", 1),
		strings.Replace(valid, `"`+ownerKeyCandidate+`"`, "123", 1),
		strings.Replace(valid, n.cfg.InstallationID, strings.Repeat("0", 64), 1),
	} {
		if _, err := decodeOwnerKey(strings.NewReader(body)); err == nil {
			t.Error("malformed request accepted")
		}
		if r := call(t, NewAPI(n), "POST", "/v1/owner-key", "192.168.50.2:4000", managedOwnerFixture, body); r.Code != 400 {
			t.Fatalf("malformed request HTTP %d", r.Code)
		}
	}
	for _, path := range []string{"/v1/owner-key?command=anything", "/v1/owner-key?"} {
		if r := call(t, NewAPI(n), "POST", path, "192.168.50.2:4000", managedOwnerFixture, valid); r.Code != 400 {
			t.Fatal("query accepted", r.Code)
		}
	}
	if req, err := decodeOwnerKey(strings.NewReader(valid + "\n")); err != nil || req != ownerKeyFixtureRequest(n) {
		t.Fatal("valid fixed request rejected", err)
	}
	assertManagedOwner(t, n)
}

func TestOwnerKeyAdmission(t *testing.T) {
	for _, scenario := range []string{"missing-owner", "foreign-owner", "unprovisioned", "foreign-installation", "legacy", "not-native", "unavailable", "active-installer", "non-LAN"} {
		t.Run(scenario, func(t *testing.T) {
			n := managedIdentityFixture(t)
			req := ownerKeyFixtureRequest(n)
			token, remote, want := managedOwnerFixture, "192.168.50.2:4000", 409
			var canceled atomic.Bool
			switch scenario {
			case "missing-owner":
				token, want = "", 401
			case "foreign-owner":
				token, want = "foreign", 401
			case "unprovisioned":
				n = newTestNode(t, "")
				if err := n.save(n.cfg); err != nil {
					t.Fatal(err)
				}
				token, want = "", 401
			case "foreign-installation":
				req.InstallationID = strings.Repeat("cd", 32)
			case "legacy":
				n.cfg.InstallationID = ""
			case "not-native":
				n.RunDir = ""
			case "unavailable":
				n.nativeStateUnavailable, want = true, 401
			case "active-installer":
				n.pdInstall = &pdInstallTask{done: make(chan struct{}), cancel: func() { canceled.Store(true) }}
			case "non-LAN":
				remote, want = "8.8.8.8:4000", 403
			}
			before, err := os.ReadFile(n.cfgPath())
			if err != nil {
				t.Fatal(err)
			}
			n.configWrite = func([]byte) error { t.Error("refusal reached persistent mutation"); return os.ErrPermission }
			r := call(t, NewAPI(n), "POST", "/v1/owner-key", remote, token, ownerKeyBody(t, req))
			if r.Code != want || canceled.Load() {
				t.Fatal("refusal changed installer or status", r.Code)
			}
			after, err := os.ReadFile(n.cfgPath())
			if err != nil || !bytes.Equal(before, after) {
				t.Fatal("refusal changed durable identity", err)
			}
			if strings.Contains(r.Body.String(), req.NewToken) {
				t.Fatal("response leaked replacement credential")
			}
		})
	}
}

func TestOwnerKeyCapabilityRequiresAvailableManagedNativeState(t *testing.T) {
	n := managedIdentityFixture(t)
	if n.Status().OwnerKeyVersion != 1 {
		t.Fatal("managed native replacement contract not advertised")
	}
	n.nativeStateUnavailable = true
	if n.Status().OwnerKeyVersion != 0 {
		t.Fatal("unavailable identity advertises replacement")
	}
	n.nativeStateUnavailable, n.RunDir = false, ""
	if n.Status().OwnerKeyVersion != 0 {
		t.Fatal("legacy persistence advertises replacement")
	}
	n.RunDir, n.cfg.InstallationID = t.TempDir(), ""
	if n.Status().OwnerKeyVersion != 0 {
		t.Fatal("unmanaged identity advertises replacement")
	}
}

func TestOwnerKeyLostReplyReloadAndIdempotence(t *testing.T) {
	for _, pending := range []bool{false, true} {
		t.Run(map[bool]string{false: "local", true: "pending-sign-in"}[pending], func(t *testing.T) {
			n := managedIdentityFixture(t)
			if pending {
				n.cfg.LocalOnly, n.cfg.PendingSignIn = false, true
				n.state = stateNeedsLogin
			}
			n.cfg.Routes = []string{"192.168.50.103"}
			share := false
			n.cfg.ShareDiscovered = &share
			if err := n.save(n.cfg); err != nil {
				t.Fatal(err)
			}
			before, state, generation := n.cfg, n.state, n.joinGen
			var closes atomic.Int32
			n.closers = []func() error{func() error { closes.Add(1); return nil }}
			channel, done := pdSessionTestPair(t, func(context.Context, byte) pdSessionReply {
				t.Error("rotation sent a DSP operation")
				return pdSessionReply{}
			})
			n.pdSession = channel
			req := ownerKeyFixtureRequest(n)
			body := ownerKeyBody(t, req)
			// Discard the successful receipt. The caller already possesses the
			// saved replacement; the server need not reproduce a new secret.
			r := call(t, NewAPI(n), "POST", "/v1/owner-key", "192.168.50.2:4000", managedOwnerFixture, body)
			if r.Code != http.StatusOK {
				t.Fatal("replacement failed", r.Code)
			}
			waitCredentialSignal(t, done)
			if channel.alive() || closes.Load() != 0 || n.state != state || n.joinGen != generation {
				t.Fatal("replacement retained PD or changed tailnet lifecycle")
			}
			before.TokenSHA256 = sha256Hex(req.NewToken)
			if !reflect.DeepEqual(before, n.cfg) || n.ownerKeyChanging || n.pdSessionCleanupConfirmed {
				t.Fatal("replacement changed unrelated configuration or claimed cleanup")
			}
			loaded := credentialReload(t, n)
			if loaded.checkToken(managedOwnerFixture) || !loaded.checkToken(req.NewToken) || loaded.Status().Claimable {
				t.Fatal("lost reply/restart changed owner admission")
			}
			if r := call(t, NewAPI(loaded), "POST", "/v1/owner-key", "192.168.50.2:4000", managedOwnerFixture, body); r.Code != 401 {
				t.Fatal("old key retained replacement authority", r.Code)
			}
			// Confirm with the saved replacement without another disk write or
			// retiring a later explicit session.
			next, _ := pdSessionTestPair(t, func(context.Context, byte) pdSessionReply { return pdSessionReply{} })
			loaded.pdSession = next
			loaded.configWrite = func([]byte) error { t.Error("confirmation wrote identity"); return os.ErrPermission }
			r = call(t, NewAPI(loaded), "POST", "/v1/owner-key", "192.168.50.2:4000", req.NewToken, body)
			var receipt ownerKeyReceipt
			if r.Code != 200 || json.Unmarshal(r.Body.Bytes(), &receipt) != nil || receipt != (ownerKeyReceipt{"owner_key_current", req.InstallationID, sha256Hex(req.NewToken)}) || !next.alive() {
				t.Fatal("replacement could not confirm exact saved candidate", r.Code)
			}
			if strings.Contains(r.Body.String(), req.NewToken) || strings.Contains(call(t, NewAPI(loaded), "GET", "/v1/status", "192.168.50.2:4000", "", "").Body.String(), sha256Hex(req.NewToken)) {
				t.Fatal("secret or owner commitment leaked in public status")
			}
		})
	}
}

func TestOwnerKeyDefinitePersistenceFailurePreservesAuthority(t *testing.T) {
	n := managedIdentityFixture(t)
	channel, _ := pdSessionTestPair(t, func(context.Context, byte) pdSessionReply { return pdSessionReply{} })
	n.pdSession = channel
	n.configWrite = func([]byte) error { return os.ErrPermission }
	r := call(t, NewAPI(n), "POST", "/v1/owner-key", "192.168.50.2:4000", managedOwnerFixture, ownerKeyBody(t, ownerKeyFixtureRequest(n)))
	if r.Code != 503 || !channel.alive() || n.ownerKeyChanging || n.nativeStateUnavailable || n.checkToken(ownerKeyCandidate) {
		t.Fatal("definite write failure did not retain old authority", r.Code)
	}
	assertManagedOwner(t, n)
	assertManagedOwner(t, credentialReload(t, n))
}

func TestOwnerKeyPendingWriteExcludesInstallerAndStart(t *testing.T) {
	n := managedIdentityFixture(t)
	h := NewAPI(n)
	var installerCalls, startCalls atomic.Int32
	n.pdInstallRun = func(context.Context, [120]byte, func()) (int, error) {
		installerCalls.Add(1)
		return 6, errors.New("unexpected launch")
	}
	n.pdSessionDial = func(context.Context, pdStartIdentity) (*pdSessionChannel, error) {
		startCalls.Add(1)
		return nil, errors.New("unexpected start")
	}
	n.configWrite = func(raw []byte) error {
		fixture := pdInstallFixture()
		if r := pdInstallCall(h, "POST", "/v1/aetherpd/install-request", managedOwnerFixture, fixture[:]); r.Code != 409 {
			t.Error("installer admitted during replacement", r.Code)
		}
		if r := call(t, h, "POST", "/v1/aetherpd/start", "192.168.50.2:4000", managedOwnerFixture, pdStartFixture); r.Code != 409 {
			t.Error("start admitted during replacement", r.Code)
		}
		return n.writeConfigFile(raw)
	}
	if _, err := n.replaceOwnerKey(managedOwnerFixture, ownerKeyFixtureRequest(n)); err != nil {
		t.Fatal(err)
	}
	if installerCalls.Load() != 0 || startCalls.Load() != 0 {
		t.Fatal("authority transition admitted a competing operation")
	}
}

func TestOwnerKeyLateStartupCannotSurviveCommit(t *testing.T) {
	n := managedIdentityFixture(t)
	h := NewAPI(n)
	entered, release := make(chan struct{}), make(chan struct{})
	channel, _ := pdSessionTestPair(t, func(context.Context, byte) pdSessionReply { return pdSessionReply{} })
	var observed context.Context
	n.pdSessionDial = func(ctx context.Context, _ pdStartIdentity) (*pdSessionChannel, error) {
		observed = ctx
		close(entered)
		<-release // Deliberately return a channel after cancellation.
		return channel, nil
	}
	var released atomic.Bool
	t.Cleanup(func() {
		if !released.Swap(true) {
			close(release)
		}
	})
	code := make(chan int, 1)
	go func() {
		code <- call(t, h, "POST", "/v1/aetherpd/start", "192.168.50.2:4000", managedOwnerFixture, pdStartFixture).Code
	}()
	waitCredentialSignal(t, entered)
	if _, err := n.replaceOwnerKey(managedOwnerFixture, ownerKeyFixtureRequest(n)); err != nil {
		t.Fatal(err)
	}
	if observed.Err() == nil {
		t.Error("replacement retained pending startup")
	}
	released.Store(true)
	close(release)
	if got := <-code; got != 401 || channel.alive() || n.pdSession != nil {
		t.Fatal("late startup escaped owner revocation", got)
	}
}

func TestOwnerKeyCompetingRequestsCannotOverwriteWinner(t *testing.T) {
	n := managedIdentityFixture(t)
	req := ownerKeyFixtureRequest(n)
	entered, release := pauseCredentialWrite(n)
	var released atomic.Bool
	t.Cleanup(func() {
		if !released.Swap(true) {
			release()
		}
	})
	first := make(chan error, 1)
	go func() { _, err := n.replaceOwnerKey(managedOwnerFixture, req); first <- err }()
	waitCredentialSignal(t, entered)
	second := make(chan error, 1)
	go func() {
		other := req
		other.NewToken = strings.Repeat("cd", 32)
		_, err := n.replaceOwnerKey(managedOwnerFixture, other)
		second <- err
	}()
	// Old-key success during a pending write is not proof of rollback.
	if !n.checkToken(managedOwnerFixture) || n.checkToken(req.NewToken) {
		t.Error("pending write published authority prematurely")
	}
	released.Store(true)
	release()
	waitCredentialResult(t, first)
	if err := <-second; !errors.Is(err, errUnauthorized) {
		t.Fatal("competing request retained old authority", err)
	}
	if !credentialReload(t, n).checkToken(req.NewToken) {
		t.Fatal("competing request replaced the committed winner")
	}
}
