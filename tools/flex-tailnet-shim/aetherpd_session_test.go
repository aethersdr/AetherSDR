package main

import (
	"context"
	"encoding/binary"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"testing"
	"time"
)

const pdStartFixture = `{"gui_client_id":"0D3EA0E8-B052-4AC6-9D32-D96918634469","gui_handle":52540638}`

func pdSessionTestPair(t *testing.T, op pdSessionOperation) (*pdSessionChannel, <-chan struct{}) {
	t.Helper()
	client, server := net.Pipe()
	done := make(chan struct{})
	go func() { defer close(done); _ = servePDSession(server, op, func() {}) }()
	client.SetReadDeadline(time.Now().Add(time.Second))
	var ready [8]byte
	if _, err := io.ReadFull(client, ready[:]); err != nil || string(ready[:]) != "PDTALK02" {
		t.Fatal(err, string(ready[:]))
	}
	client.SetReadDeadline(time.Time{})
	channel := newPDSessionChannel(client)
	channel.captureSupported = true
	t.Cleanup(func() {
		channel.Close()
		select {
		case <-done:
		case <-time.After(time.Second):
			t.Error("private server did not retire")
		}
	})
	return channel, done
}
func TestPDStartIdentityBoundary(t *testing.T) {
	id, err := decodePDStart(strings.NewReader(pdStartFixture))
	if err != nil {
		t.Fatal(err)
	}
	frame, err := id.frame()
	if err != nil || string(frame[:8]) != "PDSTART1" || binary.BigEndian.Uint32(frame[44:]) != 0x0321b4de {
		t.Fatal(err, frame)
	}
	for _, body := range []string{"", `[]`, `{}`, pdStartFixture + `{}`, strings.Repeat(" ", 513), strings.Replace(pdStartFixture, "52540638", "0", 1), strings.Replace(pdStartFixture, "52540638", "4294967296", 1), strings.Replace(pdStartFixture, "52540638", "-1", 1), strings.Replace(pdStartFixture, "52540638", "null", 1), strings.Replace(pdStartFixture, "52540638", `"0x0321b4de"`, 1), strings.Replace(pdStartFixture, "0D3EA0E8-", "0D3EA0E8/", 1), strings.Replace(pdStartFixture, "}", `,"gui_handle":123}`, 1), strings.Replace(pdStartFixture, "}", `,"command":"sh"}`, 1)} {
		if _, err := decodePDStart(strings.NewReader(body)); err == nil {
			t.Errorf("accepted %q", body)
		}
	}
}
func TestPDSessionMultiplexCancel(t *testing.T) {
	entered, release := make(chan struct{}), make(chan struct{})
	var once sync.Once
	channel, _ := pdSessionTestPair(t, func(ctx context.Context, op byte) pdSessionReply {
		if op == 'A' {
			close(entered)
			select {
			case <-release:
			case <-ctx.Done():
				return pdSessionReply{err: ctx.Err()}
			}
		}
		if op == 'X' {
			once.Do(func() { close(release) })
		}
		return pdSessionReply{code: 200, body: []byte(`{"state":"ok"}`)}
	})
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	arm := make(chan pdSessionReply, 1)
	go func() { arm <- channel.Call(ctx, 'A') }()
	<-entered
	if reply := channel.Call(ctx, 'S'); reply.err != nil || reply.code != 200 {
		t.Fatal(reply)
	}
	if reply := channel.Call(ctx, 'X'); reply.err != nil || reply.code != 200 {
		t.Fatal(reply)
	}
	if reply := <-arm; reply.err != nil || reply.code != 200 {
		t.Fatal(reply)
	}
}
func TestPDSessionDisconnectCancelsWork(t *testing.T) {
	entered := make(chan struct{})
	canceled := make(chan struct{})
	channel, done := pdSessionTestPair(t, func(ctx context.Context, op byte) pdSessionReply {
		close(entered)
		<-ctx.Done()
		close(canceled)
		return pdSessionReply{err: ctx.Err()}
	})
	ctx, cancel := context.WithCancel(context.Background())
	result := make(chan pdSessionReply, 1)
	go func() { result <- channel.Call(ctx, 'A') }()
	<-entered
	cancel()
	if reply := <-result; reply.err == nil {
		t.Fatal("canceled arm accepted")
	}
	select {
	case <-canceled:
	case <-time.After(time.Second):
		t.Fatal("caller loss did not cancel server operation")
	}
	<-done
	if channel.alive() {
		t.Fatal("canceled channel remained reusable")
	}
}
func TestPDSessionMalformedRequestStopsOwner(t *testing.T) {
	for _, mutation := range []string{"magic", "zero_id", "opcode", "reserved"} {
		t.Run(mutation, func(t *testing.T) {
			a, b := net.Pipe()
			defer a.Close()
			stopped := make(chan struct{})
			var once sync.Once
			go func() {
				_ = servePDSession(b, func(context.Context, byte) pdSessionReply {
					t.Error("invalid request reached operation")
					return pdSessionReply{}
				}, func() { once.Do(func() { close(stopped) }) })
			}()
			a.SetDeadline(time.Now().Add(time.Second))
			var ready [8]byte
			if _, err := io.ReadFull(a, ready[:]); err != nil {
				t.Fatal(err)
			}
			var frame [16]byte
			copy(frame[:8], "PDCMD001")
			binary.BigEndian.PutUint32(frame[8:12], 1)
			frame[12] = 'S'
			switch mutation {
			case "magic":
				frame[0] = 'x'
			case "zero_id":
				frame[11] = 0
			case "opcode":
				frame[12] = '!'
			case "reserved":
				frame[15] = 1
			}
			if err := pdWriteAll(a, frame[:]); err != nil {
				t.Fatal(err)
			}
			select {
			case <-stopped:
			case <-time.After(time.Second):
				t.Fatal("invalid frame retained owner")
			}
		})
	}
}
func TestPDStartAuthorizationAndRevocation(t *testing.T) {
	n := newTestNode(t, "owner")
	h := NewAPI(n)
	dials := 0
	n.pdSessionDial = func(context.Context, pdStartIdentity) (*pdSessionChannel, error) {
		dials++
		c, _ := pdSessionTestPair(t, func(context.Context, byte) pdSessionReply {
			return pdSessionReply{code: 200, body: []byte(`{"state":"idle"}`)}
		})
		return c, nil
	}
	if r := call(t, h, "POST", "/v1/aetherpd/start", "192.168.50.20:4000", "wrong", pdStartFixture); r.Code != 401 || dials != 0 {
		t.Fatal(r.Code, dials)
	}
	if r := call(t, h, "POST", "/v1/aetherpd/start", "192.168.50.20:4000", "owner", `{"command":"sh"}`); r.Code != 400 || dials != 0 {
		t.Fatal(r.Code, dials)
	}
	if r := call(t, h, "POST", "/v1/aetherpd/start", "192.168.50.20:4000", "owner", pdStartFixture); r.Code != 202 || dials != 1 {
		t.Fatal(r.Code, dials)
	}
	if r := call(t, h, "POST", "/v1/aetherpd/start", "192.168.50.20:4000", "owner", pdStartFixture); r.Code != 409 || dials != 1 {
		t.Fatal(r.Code, dials)
	}
	if r := call(t, h, "GET", "/v1/aetherpd/status", "192.168.50.20:4000", "owner", ""); r.Code != 200 {
		t.Fatal(r.Code)
	}
	channel := n.pdSession
	if err := n.SignOut("owner"); err != nil {
		t.Fatal(err)
	}
	if channel.alive() {
		t.Fatal("sign-out retained private owner")
	}
}
func TestPDStartStopAndRevocationDuringDial(t *testing.T) {
	for _, action := range []string{"stop", "signout", "rotate"} {
		t.Run(action, func(t *testing.T) {
			n := newTestNode(t, "owner")
			n.startFn = inertStart(n)
			h := NewAPI(n)
			entered, release := make(chan struct{}), make(chan struct{})
			var observed context.Context
			// Deliberately ignores cancellation: the caller must reject a late success.
			channel, _ := pdSessionTestPair(t, func(context.Context, byte) pdSessionReply { return pdSessionReply{code: 200, body: []byte(`{}`)} })
			n.pdSessionDial = func(ctx context.Context, _ pdStartIdentity) (*pdSessionChannel, error) {
				observed = ctx
				close(entered)
				<-release
				return channel, nil
			}
			result := make(chan int, 1)
			go func() {
				result <- call(t, h, "POST", "/v1/aetherpd/start", "192.168.50.20:4000", "owner", pdStartFixture).Code
			}()
			<-entered
			switch action {
			case "stop":
				if r := call(t, h, "POST", "/v1/aetherpd/stop", "192.168.50.20:4000", "owner", ""); r.Code != 202 {
					t.Fatal(r.Code)
				}
			case "signout":
				if err := n.SignOut("owner"); err != nil {
					t.Fatal(err)
				}
			case "rotate":
				if _, err := n.Provision("owner", "tskey-auth-fixture", "new-owner", nil, nil, nil); err != nil {
					t.Fatal(err)
				}
			}
			if observed.Err() == nil {
				t.Error("pending startup not canceled")
			}
			close(release)
			code := <-result
			if code != http.StatusUnauthorized && code != http.StatusServiceUnavailable {
				t.Fatal(code)
			}
			if channel.alive() || n.pdSession != nil {
				t.Fatal("late startup escaped cancellation")
			}
		})
	}
}

func TestPDSessionRejectsOversizedAndForeignReplies(t *testing.T) {
	for _, kind := range []string{"oversized", "foreign", "empty", "bad_status"} {
		t.Run(kind, func(t *testing.T) {
			a, b := net.Pipe()
			channel := newPDSessionChannel(a)
			defer channel.Close()
			defer b.Close()
			done := make(chan struct{})
			go func() {
				defer close(done)
				var request [16]byte
				if _, err := io.ReadFull(b, request[:]); err != nil {
					return
				}
				var reply [16]byte
				copy(reply[:8], "PDREP001")
				copy(reply[8:12], request[8:12])
				binary.BigEndian.PutUint16(reply[12:14], 200)
				binary.BigEndian.PutUint16(reply[14:], 2)
				switch kind {
				case "oversized":
					binary.BigEndian.PutUint16(reply[14:], pdSessionMaxBody+1)
				case "foreign":
					binary.BigEndian.PutUint32(reply[8:12], 99)
				case "empty":
					binary.BigEndian.PutUint16(reply[14:], 0)
				case "bad_status":
					binary.BigEndian.PutUint16(reply[12:14], 1)
				}
				_ = pdWriteAll(b, reply[:])
				if kind == "foreign" {
					_ = pdWriteAll(b, []byte(`{}`))
				}
			}()
			ctx, cancel := context.WithTimeout(context.Background(), time.Second)
			defer cancel()
			if reply := channel.Call(ctx, 'S'); reply.err == nil {
				t.Fatal("invalid reply accepted")
			}
			<-done
			if channel.alive() {
				t.Fatal("invalid response retained reusable channel")
			}
		})
	}
}
func TestPDSessionReplayClosesOwner(t *testing.T) {
	a, b := net.Pipe()
	defer a.Close()
	done := make(chan struct{})
	calls := 0
	go func() {
		defer close(done)
		_ = servePDSession(b, func(context.Context, byte) pdSessionReply {
			calls++
			return pdSessionReply{code: 200, body: []byte(`{}`)}
		}, func() {})
	}()
	a.SetDeadline(time.Now().Add(time.Second))
	var ready [8]byte
	if _, err := io.ReadFull(a, ready[:]); err != nil {
		t.Fatal(err)
	}
	var frame [16]byte
	copy(frame[:8], "PDCMD001")
	binary.BigEndian.PutUint32(frame[8:12], 1)
	frame[12] = 'S'
	if err := pdWriteAll(a, frame[:]); err != nil {
		t.Fatal(err)
	}
	var reply [18]byte
	if _, err := io.ReadFull(a, reply[:]); err != nil {
		t.Fatal(err)
	}
	if err := pdWriteAll(a, frame[:]); err != nil {
		t.Fatal(err)
	}
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("replayed sequence did not close owner")
	}
	if calls != 1 {
		t.Fatal("replayed command executed", calls)
	}
}
