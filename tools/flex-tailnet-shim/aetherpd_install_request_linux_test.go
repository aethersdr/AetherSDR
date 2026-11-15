//go:build linux

package main

import (
	"bytes"
	"context"
	"encoding/hex"
	"io"
	"net"
	"os"
	"path/filepath"
	"syscall"
	"testing"
	"time"

	"golang.org/x/sys/unix"
)

func TestPDInstallRAMAndCarrierAdmission(t *testing.T) {
	config := pdInstallFixture()
	public, _ := pdInstallPublic(config[:])
	file, err := pdInstallRAM(config[:])
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	var st unix.Stat_t
	var fs unix.Statfs_t
	if unix.Fstat(int(file.Fd()), &st) != nil || unix.Fstatfs(int(file.Fd()), &fs) != nil || st.Nlink != 0 || st.Mode&07777 != 0600 || st.Uid != uint32(os.Getuid()) || fs.Type != unix.TMPFS_MAGIC {
		t.Fatal("credential is not private unlinked RAM")
	}
	if _, err = file.Write([]byte("mutation")); err == nil {
		t.Fatal("credential descriptor writable")
	}
	for _, scenario := range []string{"valid", "wrong-uid", "wrong-receipt", "extra-fd", "short"} {
		t.Run(scenario, func(t *testing.T) {
			listener, err := net.ListenUnix("unixpacket", &net.UnixAddr{Name: filepath.Join(t.TempDir(), "broker"), Net: "unixpacket"})
			if err != nil {
				t.Fatal(err)
			}
			defer listener.Close()
			done := make(chan bool, 1)
			go func() {
				conn, e := listener.AcceptUnix()
				if e != nil {
					done <- false
					return
				}
				defer conn.Close()
				uid := uint32(os.Getuid())
				if scenario == "wrong-uid" {
					uid++
				}
				done <- servePDInstallConfig(conn, uid, public, file)
			}()
			conn, err := net.DialUnix("unixpacket", nil, listener.Addr().(*net.UnixAddr))
			if err != nil {
				t.Fatal(err)
			}
			defer conn.Close()
			conn.SetDeadline(time.Now().Add(time.Second))
			frame := append([]byte("PDCFG001"), public[:]...)
			var control []byte
			switch scenario {
			case "wrong-receipt":
				frame[30] ^= 1
			case "extra-fd":
				control = syscall.UnixRights(int(file.Fd()))
			case "short":
				frame = frame[:127]
			}
			conn.WriteMsgUnix(frame, control, nil)
			var data [16]byte
			oob := make([]byte, syscall.CmsgSpace(16))
			n, o, _, _, readErr := conn.ReadMsgUnix(data[:], oob)
			if scenario != "valid" {
				pdDiscardRights(oob[:o])
				if readErr == nil || n != 0 || o != 0 {
					t.Fatal("unauthorized peer received reply or credential")
				}
				if <-done {
					t.Fatal("unauthorized peer admitted")
				}
				return
			}
			if readErr != nil || n != 8 || string(data[:n]) != "PDSECR01" {
				t.Fatal(readErr, n)
			}
			messages, err := syscall.ParseSocketControlMessage(oob[:o])
			if err != nil || len(messages) != 1 {
				t.Fatal(err, len(messages))
			}
			fds, err := syscall.ParseUnixRights(&messages[0])
			if err != nil || len(fds) != 1 {
				t.Fatal(err, len(fds))
			}
			received := os.NewFile(uintptr(fds[0]), "received-config")
			defer received.Close()
			content, err := io.ReadAll(received)
			if err != nil || !bytes.Equal(content, config[:]) {
				t.Fatal("wrong secret bytes", err)
			}
			if _, err = received.Write([]byte{0}); err == nil {
				t.Fatal("received descriptor writable")
			}
			if !<-done {
				t.Fatal("valid carrier not admitted")
			}
		})
	}
}
func TestPDInstallBrokerCancellationAndRootOnly(t *testing.T) {
	if os.Getuid() == 0 {
		t.Skip("negative root admission requires unprivileged test runner")
	}
	config := pdInstallFixture()
	public, _ := pdInstallPublic(config[:])
	file, err := pdInstallRAM(config[:])
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	closeBroker, err := pdInstallConfigBroker(ctx, public, file)
	if err != nil {
		t.Fatal(err)
	}
	address := &net.UnixAddr{Name: "@aetherpd-config-" + hex.EncodeToString(public[88:]), Net: "unixpacket"}
	conn, err := net.DialUnix("unixpacket", nil, address)
	if err != nil {
		t.Fatal(err)
	}
	conn.SetDeadline(time.Now().Add(time.Second))
	conn.Write(append([]byte("PDCFG001"), public[:]...))
	var data [16]byte
	oob := make([]byte, 64)
	n, o, _, _, err := conn.ReadMsgUnix(data[:], oob)
	pdDiscardRights(oob[:o])
	conn.Close()
	if err == nil || n != 0 || o != 0 {
		t.Fatal("nonroot client received secret")
	}
	cancel()
	closeBroker()
	if conn, err := net.DialUnix("unixpacket", nil, address); err == nil {
		conn.Close()
		t.Fatal("retired credential broker still accepts clients")
	}
}
func TestPDInstallProductionIdentity(t *testing.T) {
	if os.Getuid() == 78666 {
		t.Skip("test user matches radio identity")
	}
	if _, err := runPDInstallRequest(context.Background(), pdInstallFixture(), func() { t.Error("invalid identity became ready") }); err == nil {
		t.Fatal("local user admitted by production launcher")
	}
}
