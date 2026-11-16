//go:build linux

package main

import (
	"context"
	"errors"
	"io"
	"log"
	"net"
	"os"
	"syscall"
	"time"
)

func dialPDSession(ctx context.Context, id pdStartIdentity) (*pdSessionChannel, error) {
	frame, err := id.frame()
	if err != nil {
		return nil, err
	}
	conn, err := (&net.Dialer{}).DialContext(ctx, "unix", pdSessionSocket)
	if err != nil {
		return nil, err
	}
	ok := false
	defer func() {
		if !ok {
			conn.Close()
		}
	}()
	// Socket activation is owned by PID1/root. A socket substituted by another
	// waveform user must not receive an authenticated session request.
	raw, err := conn.(*net.UnixConn).SyscallConn()
	if err != nil {
		return nil, err
	}
	var peer *syscall.Ucred
	var peerErr error
	if err = raw.Control(func(fd uintptr) {
		peer, peerErr = syscall.GetsockoptUcred(int(fd), syscall.SOL_SOCKET, syscall.SO_PEERCRED)
	}); err != nil {
		return nil, err
	}
	if peerErr != nil || peer == nil || peer.Uid != 0 {
		return nil, errors.New("session launcher peer is not root")
	}
	cancelClose := context.AfterFunc(ctx, func() { conn.Close() })
	defer cancelClose()
	deadline := time.Now().Add(20 * time.Second)
	if d, present := ctx.Deadline(); present && d.Before(deadline) {
		deadline = d
	}
	if err = conn.SetDeadline(deadline); err != nil {
		return nil, err
	}
	if err = pdWriteAll(conn, frame[:]); err != nil {
		return nil, err
	}
	var ready [8]byte
	if _, err = io.ReadFull(conn, ready[:]); err != nil {
		return nil, err
	}
	if string(ready[:]) != "PDTALK01" && string(ready[:]) != "PDTALK02" && string(ready[:]) != "PDTALK03" && string(ready[:]) != "PDTALK04" && string(ready[:]) != "PDTALK05" {
		return nil, errors.New("invalid session readiness")
	}
	if err = conn.SetDeadline(time.Time{}); err != nil {
		return nil, err
	}
	if err = ctx.Err(); err != nil {
		return nil, err
	}
	ok = true
	channel := newPDSessionChannel(conn)
	channel.captureSupported = string(ready[:]) != "PDTALK01"
	channel.correctionSupported = string(ready[:]) == "PDTALK05"
	channel.measurementSupported = string(ready[:]) == "PDTALK04" || channel.correctionSupported
	channel.testSupported = string(ready[:]) == "PDTALK03" || channel.measurementSupported
	return channel, nil
}

func runPDSessionMode() bool {
	if os.Getenv("SHIM_PD_CONTROL_FD") == "" {
		return false
	}
	if err := runInheritedPDSession(); err != nil {
		log.Printf("AetherPD private session ended: %v", err)
	}
	return true
}
func runInheritedPDSession() error {
	fd, err := inheritedPDDescriptor("SHIM_PD_CONTROL_FD")
	if err != nil {
		return err
	}
	file := os.NewFile(uintptr(fd), "aetherpd-session")
	defer file.Close()
	typ, err := syscall.GetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_TYPE)
	if err != nil || typ != syscall.SOCK_STREAM {
		return errors.New("invalid private session socket")
	}
	if _, err = syscall.Getpeername(fd); err != nil {
		return err
	}
	peer, err := syscall.GetsockoptUcred(fd, syscall.SOL_SOCKET, syscall.SO_PEERCRED)
	if err != nil || peer.Uid != 78666 || peer.Gid != 20 {
		return errors.New("invalid private session peer")
	}
	conn, err := net.FileConn(file)
	if err != nil {
		return err
	}
	defer conn.Close()
	file.Close()
	if _, ok := conn.(*net.UnixConn); !ok {
		return errors.New("private session requires Unix stream")
	}
	process, err := startInheritedAetherPD()
	if err != nil {
		return err
	}
	if process == nil {
		return errors.New("missing inherited AetherPD")
	}
	defer func() {
		process.Stop()
		ctx, cancel := context.WithTimeout(context.Background(), 7*time.Second)
		defer cancel()
		if err := process.Wait(ctx); err != nil {
			log.Printf("AetherPD cleanup: %v", err)
		}
	}()
	return servePDSession(conn, pdSessionOperations(&Node{pdProcess: process}), process.Stop, process.testUnkeyOperation)
}
