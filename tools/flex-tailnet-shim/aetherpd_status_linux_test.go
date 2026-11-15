//go:build linux

package main

import (
	"bytes"
	"context"
	"net"
	"os"
	"syscall"
	"testing"
	"time"
)

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
