package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func pdInstallFixture() [120]byte {
	var c [120]byte
	copy(c[:8], "PDXFER01")
	for i := 8; i < 40; i++ {
		c[i] = byte(i)
	}
	copy(c[40:48], "PDDELIV1")
	binary.BigEndian.PutUint32(c[48:52], 64)
	binary.BigEndian.PutUint32(c[52:56], 2)
	for i := 56; i < 120; i++ {
		c[i] = byte(i)
	}
	return c
}
func pdInstallCall(h http.Handler, method, path, token string, body []byte) *httptest.ResponseRecorder {
	r := httptest.NewRequest(method, path, bytes.NewReader(body))
	r.RemoteAddr = "192.168.1.20:54321"
	r.Header.Set("Authorization", "Bearer "+token)
	if method == "POST" {
		r.Header.Set("Content-Type", "application/octet-stream")
	}
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	return w
}
func pdInstallWait(t *testing.T, n *Node) {
	t.Helper()
	n.mu.Lock()
	task := n.pdInstall
	n.mu.Unlock()
	if task == nil {
		t.Fatal("no owned install task")
	}
	select {
	case <-task.done:
	case <-time.After(3 * time.Second):
		t.Fatal("installer did not retire")
	}
}
func TestPDInstallRequestBoundary(t *testing.T) {
	n := newTestNode(t, "owner")
	var launched atomic.Int32
	n.pdInstallRun = func(context.Context, [120]byte, func()) (int, error) { launched.Add(1); return 6, io.EOF }
	h := NewAPI(n)
	config := pdInstallFixture()
	if w := pdInstallCall(h, "POST", "/v1/aetherpd/install-request", "wrong", config[:]); w.Code != 401 {
		t.Fatal(w.Code)
	}
	for _, body := range [][]byte{nil, config[:119], append(append([]byte{}, config[:]...), 0), bytes.Repeat([]byte{'x'}, 120)} {
		if w := pdInstallCall(h, "POST", "/v1/aetherpd/install-request", "owner", body); w.Code != 400 {
			t.Fatal(w.Code, w.Body.String())
		}
	}
	for _, offset := range []int{8, 48, 52} {
		bad := config
		if offset == 8 {
			clear(bad[8:40])
		} else {
			binary.BigEndian.PutUint32(bad[offset:offset+4], 1)
		}
		if w := pdInstallCall(h, "POST", "/v1/aetherpd/install-request", "owner", bad[:]); w.Code != 400 {
			t.Fatal(offset, w.Code)
		}
	}
	r := httptest.NewRequest("POST", "/v1/aetherpd/install-request", bytes.NewReader(config[:]))
	r.Header.Set("Authorization", "Bearer owner")
	r.RemoteAddr = "192.168.1.20:1234"
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != 400 {
		t.Fatal(w.Code)
	}
	if w := pdInstallCall(h, "POST", "/v1/aetherpd/install-request?command=sh", "owner", config[:]); w.Code != 400 {
		t.Fatal(w.Code)
	}
	if launched.Load() != 0 {
		t.Fatal("invalid request reached launcher")
	}
}
func TestPDInstallRequestLifecycleAndExclusion(t *testing.T) {
	n := newTestNode(t, "owner")
	config := pdInstallFixture()
	public, _ := pdInstallPublic(config[:])
	started := make(chan struct{})
	n.pdInstallRun = func(ctx context.Context, private [120]byte, ready func()) (int, error) {
		if private != config {
			t.Error("configuration changed")
		}
		close(started)
		ready()
		<-ctx.Done()
		return 6, ctx.Err()
	}
	t.Cleanup(func() {
		n.mu.Lock()
		if n.pdInstall != nil {
			n.pdInstall.cancel()
		}
		n.mu.Unlock()
	})
	h := NewAPI(n)
	w := pdInstallCall(h, "POST", "/v1/aetherpd/install-request", "owner", config[:])
	if w.Code != 202 {
		t.Fatal(w.Code, w.Body.String())
	}
	<-started
	var response map[string]any
	if json.Unmarshal(w.Body.Bytes(), &response) != nil || response["installation_committed"] != false || response["port"] != float64(48995) {
		t.Fatal(w.Body.String())
	}
	id := response["request_id"].(string)
	n.mu.Lock()
	if n.pdInstall.public != public {
		t.Error("public request changed")
	}
	n.mu.Unlock()
	if w := pdInstallCall(h, "POST", "/v1/aetherpd/install-request", "owner", config[:]); w.Code != 409 {
		t.Fatal(w.Code)
	}
	if w := call(t, h, "POST", "/v1/aetherpd/start", "192.168.1.20:1234", "owner", pdStartFixture); w.Code != 409 {
		t.Fatal("PD start admitted while upgrading", w.Code)
	}
	path := "/v1/aetherpd/install-request/" + id
	if w := pdInstallCall(h, "DELETE", path, "wrong", nil); w.Code != 401 {
		t.Fatal(w.Code)
	}
	if w := pdInstallCall(h, "DELETE", path+"wrong", "owner", nil); w.Code != 404 {
		t.Fatal(w.Code)
	}
	if w := pdInstallCall(h, "GET", path, "owner", nil); w.Code != 200 || !strings.Contains(w.Body.String(), `"state":"running"`) {
		t.Fatal(w.Code, w.Body.String())
	}
	if w := pdInstallCall(h, "DELETE", path, "owner", nil); w.Code != 202 {
		t.Fatal(w.Code)
	}
	pdInstallWait(t, n)
	if w := pdInstallCall(h, "GET", path, "owner", nil); w.Code != 200 || !strings.Contains(w.Body.String(), `"state":"unavailable"`) || !strings.Contains(w.Body.String(), `"installation_committed":false`) {
		t.Fatal(w.Code, w.Body.String())
	}
	if w := pdInstallCall(h, "POST", "/v1/aetherpd/install-request", "owner", config[:]); w.Code != 409 {
		t.Fatal("reused request admitted", w.Code)
	}
}
func TestPDInstallRequestRevocationAndStartupDisconnect(t *testing.T) {
	for _, beforeReady := range []bool{false, true} {
		t.Run(map[bool]string{false: "revoked", true: "startup-disconnect"}[beforeReady], func(t *testing.T) {
			n := newTestNode(t, "owner")
			config := pdInstallFixture()
			entered := make(chan struct{})
			n.pdInstallRun = func(ctx context.Context, _ [120]byte, ready func()) (int, error) {
				close(entered)
				if !beforeReady {
					ready()
				}
				<-ctx.Done()
				return 6, ctx.Err()
			}
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			req := httptest.NewRequest("POST", "/v1/aetherpd/install-request", bytes.NewReader(config[:])).WithContext(ctx)
			req.Header.Set("Content-Type", "application/octet-stream")
			req.Header.Set("Authorization", "Bearer owner")
			req.RemoteAddr = "192.168.1.20:1234"
			done := make(chan struct{})
			go func() { NewAPI(n).ServeHTTP(httptest.NewRecorder(), req); close(done) }()
			<-entered
			if beforeReady {
				cancel()
			} else {
				<-done
				n.mu.Lock()
				n.retirePDSessionLocked()
				n.cfg.TokenSHA256 = sha256Hex("replacement")
				n.mu.Unlock()
			}
			pdInstallWait(t, n)
			if beforeReady {
				<-done
			}
			if !beforeReady {
				n.mu.Lock()
				id := n.pdInstall.id()
				n.mu.Unlock()
				if w := pdInstallCall(NewAPI(n), "GET", "/v1/aetherpd/install-request/"+id, "owner", nil); w.Code != 401 {
					t.Fatal("revoked credential accepted", w.Code)
				}
			}
		})
	}
}
func TestPDInstallRequestRefusesActiveSession(t *testing.T) {
	n := newTestNode(t, "owner")
	n.pdStarting = &pdSessionStart{cancel: func() {}}
	config := pdInstallFixture()
	n.pdInstallRun = func(context.Context, [120]byte, func()) (int, error) {
		t.Error("overlapping installer launched")
		return 6, io.EOF
	}
	if w := pdInstallCall(NewAPI(n), "POST", "/v1/aetherpd/install-request", "owner", config[:]); w.Code != 409 {
		t.Fatal(w.Code)
	}
}

func TestPDInstallRequestRequiresObservedCleanup(t *testing.T) {
	for _, test := range []struct {
		name, body string
		op         byte
		code       int
		admitted   bool
	}{
		{"stop_ack", `{"state":"stop_requested"}`, 'T', 202, false},
		{"cleanup_unknown", `{"available":false,"state":"cleanup_unconfirmed"}`, 'S', 200, false},
		{"receive_unknown", `{"available":false,"state":"stopped_receive_unconfirmed"}`, 'S', 200, false},
		{"stopped", `{"available":false,"state":"stopped"}`, 'S', 200, true},
		{"duplicate_state", `{"available":false,"state":"idle","state":"stopped"}`, 'S', 200, false},
	} {
		t.Run(test.name, func(t *testing.T) {
			n := newTestNode(t, "owner")
			channel, done := pdSessionTestPair(t, func(context.Context, byte) pdSessionReply {
				return pdSessionReply{code: test.code, body: []byte(test.body)}
			})
			n.pdSession = channel
			h := NewAPI(n)
			if w := call(t, aetherPDSessionHandler(n, test.op, nil), "GET", "/v1/aetherpd/status", "192.168.1.20:1234", "owner", ""); w.Code != test.code {
				t.Fatal(w.Code)
			}
			var launched atomic.Int32
			n.pdInstallRun = func(ctx context.Context, _ [120]byte, ready func()) (int, error) {
				launched.Add(1)
				if channel.alive() {
					t.Error("installer started before retiring stopped lifetime channel")
				}
				ready()
				<-ctx.Done()
				return 6, ctx.Err()
			}
			t.Cleanup(func() {
				n.mu.Lock()
				if n.pdInstall != nil {
					n.pdInstall.cancel()
				}
				n.mu.Unlock()
			})
			config := pdInstallFixture()
			w := pdInstallCall(h, "POST", "/v1/aetherpd/install-request", "owner", config[:])
			if !test.admitted {
				if w.Code != 409 || launched.Load() != 0 || !channel.alive() || n.pdSession != channel {
					t.Fatal("unconfirmed cleanup crossed upgrade fence", w.Code, launched.Load(), channel.alive())
				}
				return
			}
			if w.Code != 202 || launched.Load() != 1 || n.pdSession != nil || channel.alive() || n.pdSessionCleanupConfirmed {
				t.Fatal("confirmed cleanup was not retired", w.Code, w.Body.String())
			}
			select {
			case <-done:
			case <-time.After(time.Second):
				t.Fatal("stopped private server retained")
			}
			if w := call(t, h, "POST", "/v1/aetherpd/start", "192.168.1.20:1234", "owner", pdStartFixture); w.Code != 409 {
				t.Fatal("new session admitted during install", w.Code)
			}
		})
	}
}

func TestPDInstallRequestKeepsBusyCleanupChannel(t *testing.T) {
	n := newTestNode(t, "owner")
	entered, release := make(chan struct{}), make(chan struct{})
	channel, _ := pdSessionTestPair(t, func(ctx context.Context, op byte) pdSessionReply {
		if op == 'A' {
			close(entered)
			select {
			case <-release:
			case <-ctx.Done():
			}
		}
		return pdSessionReply{code: 200, body: []byte(`{"available":false,"state":"stopped"}`)}
	})
	n.pdSession = channel
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	pending := make(chan pdSessionReply, 1)
	go func() { pending <- channel.Call(ctx, 'A') }()
	<-entered
	if w := call(t, aetherPDSessionHandler(n, 'S', nil), "GET", "/v1/aetherpd/status", "192.168.1.20:1234", "owner", ""); w.Code != 200 || !n.pdSessionCleanupConfirmed {
		t.Fatal("missing cleanup observation", w.Code)
	}
	n.pdInstallRun = func(context.Context, [120]byte, func()) (int, error) {
		t.Error("busy channel admitted installer")
		return 6, io.EOF
	}
	config := pdInstallFixture()
	w := pdInstallCall(NewAPI(n), "POST", "/v1/aetherpd/install-request", "owner", config[:])
	if w.Code != 409 || !channel.alive() {
		t.Fatal("upgrade overtook pending operation", w.Code)
	}
	close(release)
	if reply := <-pending; reply.err != nil {
		t.Fatal(reply.err)
	}
}

func TestPDStopAcknowledgmentRevokesCleanupObservation(t *testing.T) {
	n := newTestNode(t, "owner")
	channel, _ := pdSessionTestPair(t, func(_ context.Context, op byte) pdSessionReply {
		if op == 'T' {
			return pdSessionReply{code: 202, body: []byte(`{"state":"stop_requested"}`)}
		}
		return pdSessionReply{code: 200, body: []byte(`{"available":false,"state":"stopped"}`)}
	})
	n.pdSession = channel
	for _, op := range []byte{'S', 'T'} {
		call(t, aetherPDSessionHandler(n, op, nil), "GET", "/v1/aetherpd/status", "192.168.1.20:1234", "owner", "")
		if n.pdSessionCleanupConfirmed != (op == 'S') {
			t.Fatal("cleanup receipt survived Stop acknowledgment", op)
		}
	}
}
