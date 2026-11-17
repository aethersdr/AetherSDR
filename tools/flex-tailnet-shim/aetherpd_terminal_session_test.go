package main

import (
	"context"
	"testing"
	"time"
)

func TestPDTerminalSessionExplicitRestart(t *testing.T) {
	for _, test := range []struct {
		name, body string
		code       int
		op         byte
		ended      bool
	}{
		{"stopped", `{"available":false,"state":"stopped"}`, 200, 'S', true},
		{"crashed", `{"available":false,"state":"cleanup_unconfirmed"}`, 200, 'S', true},
		{"receive_unknown", `{"available":false,"state":"stopped_receive_unconfirmed"}`, 200, 'S', true},
		{"reordered", " { \"state\":\"stopped\", \"available\":false }\n", 200, 'S', true},
		{"unavailable", `{"available":false,"state":"unavailable"}`, 200, 'S', false},
		{"still_available", `{"available":true,"state":"stopped"}`, 200, 'S', false},
		{"missing_available", `{"state":"stopped"}`, 200, 'S', false},
		{"null_available", `{"available":null,"state":"stopped"}`, 200, 'S', false},
		{"wrong_available", `{"available":"false","state":"stopped"}`, 200, 'S', false},
		{"duplicate_available", `{"available":true,"available":false,"state":"stopped"}`, 200, 'S', false},
		{"duplicate_state", `{"available":false,"state":"idle","state":"stopped"}`, 200, 'S', false},
		{"extra", `{"available":false,"state":"stopped","keyed":true}`, 200, 'S', false},
		{"trailing", `{"available":false,"state":"stopped"}{}`, 200, 'S', false},
		{"malformed", `{"available":false,"state":"stopped"`, 200, 'S', false},
		{"wrong_code", `{"available":false,"state":"stopped"}`, 503, 'S', false},
		{"wrong_operation", `{"available":false,"state":"stopped"}`, 200, 'X', false},
	} {
		t.Run(test.name, func(t *testing.T) {
			n := newTestNode(t, "owner")
			channel, _ := pdSessionTestPair(t, func(context.Context, byte) pdSessionReply {
				return pdSessionReply{code: test.code, body: []byte(test.body)}
			})
			n.pdSession = channel
			dials := 0
			n.pdSessionDial = func(context.Context, pdStartIdentity) (*pdSessionChannel, error) {
				dials++
				if channel.alive() {
					t.Error("successor dial before retiring terminal channel")
				}
				next, _ := pdSessionTestPair(t, func(context.Context, byte) pdSessionReply {
					return pdSessionReply{code: 200, body: []byte(`{"available":true,"state":"idle"}`)}
				})
				return next, nil
			}
			h := aetherPDSessionHandler(n, test.op, nil)
			if r := call(t, h, "GET", "/v1/aetherpd/status", "192.168.50.20:4000", "owner", ""); r.Code != test.code {
				t.Fatal(r.Code)
			}
			confirmed := test.ended && (test.name == "stopped" || test.name == "reordered")
			if n.pdSessionCleanupConfirmed != confirmed {
				t.Fatalf("cleanup confirmed=%v want=%v", n.pdSessionCleanupConfirmed, confirmed)
			}
			if n.pdSessionStopped != test.ended || !channel.alive() || dials != 0 {
				t.Fatalf("terminal observation: stopped=%v alive=%v dials=%d", n.pdSessionStopped, channel.alive(), dials)
			}
			r := call(t, aetherPDStartHandler(n), "POST", "/v1/aetherpd/start", "192.168.50.20:4000", "owner", pdStartFixture)
			want, wantDials := 409, 0
			if test.ended {
				want, wantDials = 202, 1
			}
			if r.Code != want || dials != wantDials {
				t.Fatalf("explicit restart: code=%d body=%s dials=%d", r.Code, r.Body.String(), dials)
			}
		})
	}
}

func TestPDTerminalReplyCannotRetireSuccessor(t *testing.T) {
	n := newTestNode(t, "owner")
	entered, release := make(chan struct{}), make(chan struct{})
	old, _ := pdSessionTestPair(t, func(context.Context, byte) pdSessionReply {
		close(entered)
		<-release
		return pdSessionReply{code: 200, body: []byte(`{"available":false,"state":"stopped"}`)}
	})
	next, _ := pdSessionTestPair(t, func(context.Context, byte) pdSessionReply {
		return pdSessionReply{code: 200, body: []byte(`{"available":true,"state":"idle"}`)}
	})
	n.pdSession = old
	result := make(chan int, 1)
	go func() {
		result <- call(t, aetherPDSessionHandler(n, 'S', nil), "GET", "/v1/aetherpd/status", "192.168.50.20:4000", "owner", "").Code
	}()
	<-entered
	n.mu.Lock()
	n.pdSession = next
	n.mu.Unlock()
	close(release)
	if code := <-result; code != 503 || n.pdSessionStopped || n.pdSessionCleanupConfirmed || !next.alive() {
		t.Fatalf("late terminal reply reached successor: code=%d stopped=%v alive=%v", code, n.pdSessionStopped, next.alive())
	}
}

func TestPDTerminalSessionKeepsPendingOperationFence(t *testing.T) {
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
	if r := call(t, aetherPDSessionHandler(n, 'S', nil), "GET", "/v1/aetherpd/status", "192.168.50.20:4000", "owner", ""); r.Code != 200 || !n.pdSessionStopped {
		t.Fatal("terminal status not observed", r.Code)
	}
	dials := 0
	n.pdSessionDial = func(context.Context, pdStartIdentity) (*pdSessionChannel, error) {
		dials++
		return nil, nil
	}
	if r := call(t, aetherPDStartHandler(n), "POST", "/v1/aetherpd/start", "192.168.50.20:4000", "owner", pdStartFixture); r.Code != 409 || dials != 0 || !channel.alive() {
		t.Fatal("restart overtook pending operation", r.Code, dials)
	}
	close(release)
	if r := <-pending; r.err != nil {
		t.Fatal(r.err)
	}
}

func TestPDTerminalReplyAfterRevocation(t *testing.T) {
	n := newTestNode(t, "owner")
	entered, release := make(chan struct{}), make(chan struct{})
	channel, _ := pdSessionTestPair(t, func(context.Context, byte) pdSessionReply {
		close(entered)
		<-release
		return pdSessionReply{code: 200, body: []byte(`{"available":false,"state":"stopped"}`)}
	})
	n.pdSession = channel
	result := make(chan int, 1)
	go func() {
		result <- call(t, aetherPDSessionHandler(n, 'S', nil), "GET", "/v1/aetherpd/status", "192.168.50.20:4000", "owner", "").Code
	}()
	<-entered
	if err := n.SignOut("owner"); err != nil {
		t.Fatal(err)
	}
	close(release)
	if code := <-result; code != 401 || n.pdSessionStopped || n.pdSessionCleanupConfirmed || channel.alive() {
		t.Fatalf("revoked terminal reply accepted: code=%d stopped=%v alive=%v", code, n.pdSessionStopped, channel.alive())
	}
}
