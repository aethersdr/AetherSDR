//go:build linux

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net"
	"os"
	"strings"
	"syscall"
	"testing"
	"time"
)

func TestAetherPDCompletionWaitsForCleanup(t *testing.T) {
	for _, failure := range []string{"none", "dsp", "unkey", "lease", "receive", "setup", "deadline", "receive_deadline",
		"receive+dsp", "receive+unkey", "receive+lease", "receive+setup"} {
		t.Run(failure, func(t *testing.T) {
			injected := func(role string) error {
				for _, part := range strings.Split(failure, "+") {
					if part == role {
						return errors.New("injected " + role + " failure")
					}
				}
				return nil
			}
			ownerRead, ownerWrite, err := os.Pipe()
			if err != nil {
				t.Fatal(err)
			}
			defer ownerRead.Close()
			defer ownerWrite.Close()
			setupRead, setupWrite, err := os.Pipe()
			if err != nil {
				t.Fatal(err)
			}
			defer setupRead.Close()
			defer setupWrite.Close()
			status, _ := privateStatusFixture(t)
			calibration, _ := privateStatusFixture(t)
			guard := &aetherPDUnkey{done: make(chan struct{}), err: injected("unkey")}
			receive := &aetherPDLeaseGuard{done: make(chan struct{}), err: injected("receive")}
			lease := &aetherPDLeaseGuard{done: make(chan struct{}), err: injected("lease"), receive: receive}
			setup := &pdSetupChildren{done: make(chan struct{}), failed: make(chan struct{}), errs: []error{injected("setup")}}
			wait := func() error {
				_, err := io.Copy(io.Discard, ownerRead)
				return errors.Join(err, injected("dsp"))
			}
			p := monitorAetherPDProcessWithSetup(0, wait, ownerWrite, status.status, calibration.status,
				setupWrite, guard, pdReceiveOriginal{}, lease, setup)
			defer p.Stop()
			operation := pdSessionOperations(&Node{pdProcess: p})
			ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
			defer cancel()
			ack := operation(ctx, 'T')
			if ack.code != 202 {
				t.Fatal("Stop not acknowledged", ack)
			}
			// Socket closure and a Stop receipt do not establish worker cleanup.
			p.status.Close()
			if got := operation(ctx, 'S'); got.code != 503 {
				t.Fatal("premature completion while guardians are outstanding", got)
			}
			close(guard.done)
			close(lease.done)
			if failure != "receive_deadline" {
				close(receive.done)
			} else {
				defer close(receive.done)
			}
			if failure != "deadline" {
				close(setup.done)
			} else {
				defer close(setup.done)
			}
			err = p.Wait(ctx)
			if ctx.Err() != nil || (err == nil) != (failure == "none") {
				t.Fatal("unexpected supervisor completion", err, ctx.Err())
			}
			got := operation(ctx, 'S')
			var body map[string]any
			want := "stopped"
			if failure != "none" {
				want = "cleanup_unconfirmed"
			}
			if failure == "receive" {
				want = "stopped_receive_unconfirmed"
			}
			if got.code != 200 || json.Unmarshal(got.body, &body) != nil || len(body) != 2 ||
				body["available"] != false || body["state"] != want {
				t.Fatalf("wrong supervisor status: %d %s", got.code, got.body)
			}
			if _, err := p.readNativeStatus(ctx); err == nil {
				t.Fatal("terminal summary fabricated native DSP data")
			}
			cancel()
			if _, err := p.queryStatus(ctx); err == nil {
				t.Fatal("canceled query succeeded")
			}
		})
	}
}

func privateStatusFixture(t *testing.T) (*aetherPDProcess, *net.UnixConn) {
	t.Helper()
	fds, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_DGRAM|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		t.Fatal(err)
	}
	connections := make([]*net.UnixConn, 2)
	for i, fd := range fds {
		file := os.NewFile(uintptr(fd), "status-fixture")
		conn, err := net.FileConn(file)
		file.Close()
		if err != nil {
			t.Fatal(err)
		}
		connections[i] = conn.(*net.UnixConn)
		t.Cleanup(func() { conn.Close() })
	}
	p := &aetherPDProcess{status: connections[0], statusReads: make(chan struct{}, 1), done: make(chan struct{})}
	_ = connections[1].SetDeadline(time.Now().Add(3 * time.Second))
	return p, connections[1]
}

func TestPrivateAetherPDStatusIgnoresLateReply(t *testing.T) {
	p, peer := privateStatusFixture(t)
	first := make(chan []byte, 1)
	done := make(chan error, 1)
	go func() {
		buffer := make([]byte, 9)
		if _, err := peer.Read(buffer); err != nil {
			done <- err
			return
		}
		first <- append([]byte(nil), buffer...)
		second := make([]byte, 9)
		if _, err := peer.Read(second); err != nil {
			done <- err
			return
		}
		if _, err := peer.Write(append(buffer[1:], []byte(`{"old":true}`)...)); err != nil {
			done <- err
			return
		}
		_, err := peer.Write(append(second[1:], []byte(`{"new":true}`)...))
		done <- err
	}()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	failed := make(chan error, 1)
	go func() { _, err := p.readNativeStatus(ctx); failed <- err }()
	select {
	case <-first:
	case err := <-failed:
		t.Fatal("first query failed before sending", err)
	case <-time.After(2 * time.Second):
		t.Fatal("first request did not arrive")
	}
	cancel()
	select {
	case err := <-failed:
		if err == nil {
			t.Fatal("cancelled read succeeded")
		}
	case <-time.After(time.Second):
		t.Fatal("cancellation blocked")
	}
	data, err := p.readNativeStatus(context.Background())
	if err != nil || !bytes.Equal(data, []byte(`{"new":true}`)) {
		t.Fatal(string(data), err)
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}

func TestPrivateAetherPDStatusRejectsTruncation(t *testing.T) {
	p, peer := privateStatusFixture(t)
	go func() {
		request := make([]byte, 9)
		if _, err := peer.Read(request); err == nil {
			_, _ = peer.Write(append(request[1:], bytes.Repeat([]byte("x"), aetherPDMaxStatusBytes+2)...))
		}
	}()
	if _, err := p.readNativeStatus(context.Background()); err == nil {
		t.Fatal("oversized response accepted")
	}
}

func TestPrivateAetherPDStatusStopsWithOwnedSession(t *testing.T) {
	p, peer := privateStatusFixture(t)
	_ = peer.SetReadDeadline(time.Now().Add(30 * time.Millisecond))
	close(p.done)
	if _, err := p.readNativeStatus(context.Background()); err == nil {
		t.Fatal("dead owned session accepted")
	}
	if _, err := peer.Read(make([]byte, 10)); err == nil {
		t.Fatal("dead session sent request")
	}
	p, _ = privateStatusFixture(t)
	p.statusNonce = ^uint64(0)
	for i := 0; i < 2; i++ {
		if _, err := p.readNativeStatus(context.Background()); err == nil {
			t.Fatal("counter wrap accepted")
		}
	}
}
