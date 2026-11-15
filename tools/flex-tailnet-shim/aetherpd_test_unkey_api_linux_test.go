//go:build linux

package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func pdTestChannelPair(t *testing.T, op pdSessionTestOperation, stop func()) *pdSessionChannel {
	t.Helper()
	client, server := net.Pipe()
	done := make(chan struct{})
	go func() {
		defer close(done)
		_ = servePDSession(server, func(context.Context, byte) pdSessionReply {
			return pdCaptureJSON(200, map[string]string{"state": "idle"})
		}, stop, op)
	}()
	client.SetReadDeadline(time.Now().Add(time.Second))
	var ready [8]byte
	if _, err := io.ReadFull(client, ready[:]); err != nil || string(ready[:]) != "PDTALK03" {
		t.Fatal(err, string(ready[:]))
	}
	client.SetReadDeadline(time.Time{})
	channel := newPDSessionChannel(client)
	channel.captureSupported = true
	channel.testSupported = true
	t.Cleanup(func() {
		channel.Close()
		select {
		case <-done:
		case <-time.After(time.Second):
			t.Error("test session did not retire")
		}
	})
	return channel
}

func pdTestOperationFixture(t *testing.T, s *pdTestUnkeys) pdSessionTestOperation {
	return func(ctx context.Context, op byte, id [16]byte, duration uint16) pdSessionReply {
		return s.operation(ctx, op, id, duration, func(runCtx context.Context, ms int) (*pdTestUnkeyRun, error) {
			return s.start(runCtx, func(childCtx context.Context) (*pdBoundedUnkey, error) {
				b := testUnkeyHelper(t, childCtx)
				now, err := pdMonotonicMS()
				if err != nil {
					return nil, err
				}
				b.guard.cleanupDue = now + int64(ms)
				go func() { <-childCtx.Done(); close(b.guard.done) }()
				return b, nil
			})
		})
	}
}

func TestPDTestDurationAndObservationBoundaries(t *testing.T) {
	for _, body := range []string{"", `{}`, `[]`, `{"cleanup_after_ms":0}`, `{"cleanup_after_ms":15001}`, `{"cleanup_after_ms":null}`, `{"cleanup_after_ms":"1000"}`, `{"cleanup_after_ms":1.0}`, `{"Cleanup_After_MS":1}`, `{"cleanup_after_ms":1,"cleanup_after_ms":2}`, `{"cleanup_after_ms":1,"command":"xmit 1"}`, `{"cleanup_after_ms":1}{}`, strings.Repeat(" ", 129)} {
		if _, err := decodePDTestDuration(strings.NewReader(body)); err == nil {
			t.Fatal("invalid duration accepted", body)
		}
	}
	for _, duration := range []int{1, 15000} {
		if got, err := decodePDTestDuration(strings.NewReader(fmt.Sprintf(`{"cleanup_after_ms":%d}`, duration))); err != nil || int(got) != duration {
			t.Fatal(got, err)
		}
	}
	good := `{"test_id":"0123456789abcdef0123456789abcdef","state":"observing","cleanup_due_ms":"123456","remaining_ms":123}`
	if _, err := decodePDTestObservation([]byte(good)); err != nil {
		t.Fatal(err)
	}
	for _, body := range []string{good + `{}`, strings.Replace(good, `"state":`, `"state":"ended","state":`, 1), strings.Replace(good, `123}`, `null}`, 1), strings.Replace(good, `"123456"`, `"+123456"`, 1), strings.Replace(good, `123}`, `0}`, 1), strings.Replace(good, `123}`, `15001}`, 1), strings.Replace(good, `"observing"`, `"ended"`, 1), strings.Replace(good, `"test_id"`, `"TEST_ID"`, 1)} {
		if _, err := decodePDTestObservation([]byte(body)); err == nil {
			t.Fatal("invalid observation accepted", body)
		}
	}
	legacy := &pdSessionChannel{}
	if reply := legacy.CallTest(context.Background(), pdTestBegin, [16]byte{}, 15000); reply.err == nil {
		t.Fatal("legacy test request sent")
	}
}

func TestPDTestPublicLifecycle(t *testing.T) {
	n := newTestNode(t, "owner")
	var s pdTestUnkeys
	n.pdSession = pdTestChannelPair(t, pdTestOperationFixture(t, &s), s.close)
	h := NewAPI(n)
	begin := func() pdTestObservation {
		response := call(t, h, "POST", "/v1/aetherpd/test-unkey", "192.168.50.20:4000", "owner", `{"cleanup_after_ms":15000}`)
		observation, err := decodePDTestObservation(response.Body.Bytes())
		if response.Code != 200 || err != nil || observation.State != "observing" {
			t.Fatal(response.Code, response.Body.String(), err)
		}
		return observation
	}
	first := begin()
	if response := call(t, h, "POST", "/v1/aetherpd/test-unkey", "192.168.50.20:4000", "owner", `{"cleanup_after_ms":15000}`); response.Code != 409 {
		t.Fatal("overlap not refused", response.Code)
	}
	for i := 0; i < 2; i++ {
		response := call(t, h, "GET", "/v1/aetherpd/test-unkey/"+first.TestID, "192.168.50.20:4000", "owner", "")
		observation, err := decodePDTestObservation(response.Body.Bytes())
		if response.Code != 200 || err != nil || observation.State != "observing" || observation.CleanupDueMS != first.CleanupDueMS || observation.RemainingMS > first.RemainingMS {
			t.Fatal("request completion or polling altered run", response.Code, observation, err)
		}
	}
	if response := call(t, h, "POST", "/v1/aetherpd/test-unkey/"+first.TestID+"/stop", "192.168.50.20:4000", "owner", ""); response.Code != 202 {
		t.Fatal(response.Code, response.Body.String())
	}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err := s.wait(ctx); err != nil {
		t.Fatal(err)
	}
	second := begin()
	if second.TestID == first.TestID {
		t.Fatal("test ID reused")
	}
	if response := call(t, h, "POST", "/v1/aetherpd/test-unkey/"+first.TestID+"/stop", "192.168.50.20:4000", "owner", ""); response.Code != 404 {
		t.Fatal("stale stop reached successor", response.Code)
	}
	if response := call(t, h, "GET", "/v1/aetherpd/test-unkey/"+second.TestID, "192.168.50.20:4000", "owner", ""); response.Code != 200 || !bytes.Contains(response.Body.Bytes(), []byte(`"observing"`)) {
		t.Fatal("successor lost after stale stop", response.Body.String())
	}
	n.pdSession.Close()
	ctx2, cancel2 := context.WithTimeout(context.Background(), time.Second)
	defer cancel2()
	// The server owns teardown; wait for its close before waiting for the child.
	deadline := time.After(time.Second)
	for {
		s.mu.Lock()
		closed := s.closed
		s.mu.Unlock()
		if closed {
			break
		}
		select {
		case <-deadline:
			t.Fatal("session retained helper")
		default:
			time.Sleep(time.Millisecond)
		}
	}
	if err := s.wait(ctx2); err != nil {
		t.Fatal(err)
	}
}

func TestPDTestPublicBoundary(t *testing.T) {
	n := newTestNode(t, "owner")
	var calls atomic.Int32
	n.pdSession = pdTestChannelPair(t, func(context.Context, byte, [16]byte, uint16) pdSessionReply {
		calls.Add(1)
		return pdCaptureJSON(503, map[string]string{"state": "unavailable"})
	}, func() {})
	h := NewAPI(n)
	for _, c := range []struct {
		method, path, token, body string
		code                      int
	}{
		{"POST", "/v1/aetherpd/test-unkey", "wrong", `{"cleanup_after_ms":15000}`, 401},
		{"POST", "/v1/aetherpd/test-unkey?x=1", "owner", `{"cleanup_after_ms":15000}`, 400},
		{"POST", "/v1/aetherpd/test-unkey", "owner", `{"cleanup_after_ms":15001}`, 400},
		{"GET", "/v1/aetherpd/test-unkey/not-an-id", "owner", "", 400},
		{"POST", "/v1/aetherpd/test-unkey/00000000000000000000000000000000/stop", "owner", "", 400},
		{"POST", "/v1/aetherpd/test-unkey/0123456789abcdef0123456789abcdef/stop", "owner", "{}", 400},
	} {
		if response := call(t, h, c.method, c.path, "192.168.50.20:4000", c.token, c.body); response.Code != c.code {
			t.Fatal(c, response.Code)
		}
	}
	if calls.Load() != 0 {
		t.Fatal("invalid request reached private session")
	}
	n.mu.Lock()
	n.pdSessionStopped = true
	n.mu.Unlock()
	if response := call(t, h, "POST", "/v1/aetherpd/test-unkey", "192.168.50.20:4000", "owner", `{"cleanup_after_ms":15000}`); response.Code != 503 || calls.Load() != 0 {
		t.Fatal("stopped session admitted helper")
	}
}

func TestPDTestPublicOwnerFence(t *testing.T) {
	for _, revoke := range []bool{false, true} {
		t.Run(fmt.Sprint(revoke), func(t *testing.T) {
			n := newTestNode(t, "owner")
			entered, release := make(chan struct{}), make(chan struct{})
			var s pdTestUnkeys
			op := pdTestOperationFixture(t, &s)
			channel := pdTestChannelPair(t, func(ctx context.Context, b byte, id [16]byte, ms uint16) pdSessionReply {
				close(entered)
				<-release
				return op(ctx, b, id, ms)
			}, s.close)
			n.pdSession = channel
			h := NewAPI(n)
			done := make(chan *httptest.ResponseRecorder, 1)
			go func() {
				done <- call(t, h, "POST", "/v1/aetherpd/test-unkey", "192.168.50.20:4000", "owner", `{"cleanup_after_ms":15000}`)
			}()
			<-entered
			n.mu.Lock()
			if revoke {
				n.cfg.TokenSHA256 = sha256Hex("replacement")
			} else {
				n.pdSession = nil
			}
			n.mu.Unlock()
			close(release)
			response := <-done
			want := 503
			if revoke {
				want = 401
			}
			if response.Code != want || channel.alive() || bytes.Contains(response.Body.Bytes(), []byte("test_id")) {
				t.Fatal("late test escaped owner fence", response.Code)
			}
		})
	}
}

func TestPDTestMalformedPrivateRequest(t *testing.T) {
	for _, kind := range []string{"zero-status-id", "begin-with-id", "too-long", "duration-on-stop"} {
		t.Run(kind, func(t *testing.T) {
			client, server := net.Pipe()
			defer client.Close()
			done := make(chan struct{})
			go func() {
				defer close(done)
				_ = servePDSession(server, func(context.Context, byte) pdSessionReply {
					t.Error("legacy operation called")
					return pdSessionReply{}
				}, func() {}, func(context.Context, byte, [16]byte, uint16) pdSessionReply {
					t.Error("malformed test reached operation")
					return pdSessionReply{}
				})
			}()
			client.SetDeadline(time.Now().Add(time.Second))
			var hello [8]byte
			if _, err := io.ReadFull(client, hello[:]); err != nil {
				t.Fatal(err)
			}
			var frame [34]byte
			copy(frame[:8], "PDCMD002")
			binary.BigEndian.PutUint32(frame[8:12], 1)
			frame[12] = pdTestStatus
			switch kind {
			case "begin-with-id":
				frame[12] = pdTestBegin
				frame[16] = 1
				binary.BigEndian.PutUint16(frame[32:], 1000)
			case "too-long":
				frame[12] = pdTestBegin
				binary.BigEndian.PutUint16(frame[32:], 15001)
			case "duration-on-stop":
				frame[12] = pdTestStop
				frame[16] = 1
				binary.BigEndian.PutUint16(frame[32:], 1)
			}
			if err := pdWriteAll(client, frame[:]); err != nil {
				t.Fatal(err)
			}
			select {
			case <-done:
			case <-time.After(time.Second):
				t.Fatal("malformed request retained session")
			}
		})
	}
}

func TestPDTestCanceledBeginClosesSession(t *testing.T) {
	entered := make(chan struct{})
	canceled := make(chan struct{})
	channel := pdTestChannelPair(t, func(ctx context.Context, _ byte, _ [16]byte, _ uint16) pdSessionReply {
		close(entered)
		<-ctx.Done()
		close(canceled)
		return pdSessionReply{err: ctx.Err()}
	}, func() {})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan pdSessionReply, 1)
	go func() { done <- channel.CallTest(ctx, pdTestBegin, [16]byte{}, 15000) }()
	<-entered
	cancel()
	if reply := <-done; reply.err == nil {
		t.Fatal("canceled begin succeeded")
	}
	select {
	case <-canceled:
	case <-time.After(time.Second):
		t.Fatal("server begin not canceled")
	}
	if channel.alive() {
		t.Fatal("canceled begin left reusable channel")
	}
}

func TestPDTestStatusCancellationKeepsDeadlineOwner(t *testing.T) {
	entered, release := make(chan struct{}), make(chan struct{})
	channel := pdTestChannelPair(t, func(ctx context.Context, op byte, _ [16]byte, _ uint16) pdSessionReply {
		if op == pdTestStatus {
			close(entered)
			select {
			case <-release:
			case <-ctx.Done():
				return pdSessionReply{err: ctx.Err()}
			}
		}
		return pdCaptureJSON(200, map[string]string{"state": "ignored"})
	}, func() {})
	id, _ := pdTestID("0123456789abcdef0123456789abcdef")
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan pdSessionReply, 1)
	go func() { done <- channel.CallTest(ctx, pdTestStatus, id, 0) }()
	<-entered
	cancel()
	if reply := <-done; reply.err == nil {
		t.Fatal("canceled status succeeded")
	}
	if !channel.alive() {
		t.Fatal("read cancellation closed correction session")
	}
	close(release)
	if reply := channel.Call(context.Background(), 'S'); reply.err != nil || reply.code != 200 {
		t.Fatal("late status corrupted session", reply)
	}
}

func TestPDTestObservationRoundTrip(t *testing.T) {
	data, err := json.Marshal(pdTestObservation{"0123456789abcdef0123456789abcdef", "ended", "123", 0})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := decodePDTestObservation(data); err != nil {
		t.Fatal(err)
	}
}

func TestPDTestCanceledStaleStopKeepsSuccessor(t *testing.T) {
	n := newTestNode(t, "owner")
	entered, release := make(chan struct{}), make(chan struct{})
	var s pdTestUnkeys
	op := pdTestOperationFixture(t, &s)
	channel := pdTestChannelPair(t, func(ctx context.Context, b byte, id [16]byte, ms uint16) pdSessionReply {
		if b == pdTestStop {
			close(entered)
			<-release
		}
		return op(ctx, b, id, ms)
	}, s.close)
	n.pdSession = channel
	h := NewAPI(n)
	response := call(t, h, "POST", "/v1/aetherpd/test-unkey", "192.168.50.20:4000", "owner", `{"cleanup_after_ms":15000}`)
	active, err := decodePDTestObservation(response.Body.Bytes())
	if response.Code != 200 || err != nil {
		t.Fatal(response.Code, err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	request := httptest.NewRequest("POST", "http://shim/v1/aetherpd/test-unkey/0123456789abcdef0123456789abcdef/stop", nil).WithContext(ctx)
	request.RemoteAddr = "192.168.50.20:4000"
	request.Header.Set("Authorization", "Bearer owner")
	done := make(chan struct{})
	go func() { defer close(done); h.ServeHTTP(httptest.NewRecorder(), request) }()
	<-entered
	cancel()
	<-done
	if !channel.alive() {
		t.Fatal("canceled stale stop retired current session")
	}
	close(release)
	response = call(t, h, "GET", "/v1/aetherpd/test-unkey/"+active.TestID, "192.168.50.20:4000", "owner", "")
	current, err := decodePDTestObservation(response.Body.Bytes())
	if response.Code != 200 || err != nil || current.State != "observing" || current.CleanupDueMS != active.CleanupDueMS {
		t.Fatal("successor lost after late stale stop", response.Code, err)
	}
}
