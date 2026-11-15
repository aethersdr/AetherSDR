//go:build linux

package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net"
	"os"
	"os/exec"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
)

func pdInstallIdentity() error {
	if os.Getuid() != 78666 || os.Geteuid() != 78666 || os.Getgid() != 20 || os.Getegid() != 20 {
		return errors.New("installer requires the unprivileged waveform identity")
	}
	var header unix.CapUserHeader
	header.Version = unix.LINUX_CAPABILITY_VERSION_3
	var caps [2]unix.CapUserData
	if err := unix.Capget(&header, &caps[0]); err != nil {
		return err
	}
	for _, c := range caps {
		if c.Effective|c.Permitted|c.Inheritable != 0 {
			return errors.New("installer parent retains capabilities")
		}
	}
	return nil
}

// Open the fixed boot-verified runtime binary through protected directories.
// No request field selects an executable, argument, path or environment value.
func pdInstallProgram() (*os.File, error) {
	fd, err := unix.Open("/", unix.O_RDONLY|unix.O_DIRECTORY|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	for i, part := range []string{"run", "aetherpd", "bin", "pd-primary-launch"} {
		flags := unix.O_RDONLY | unix.O_NOFOLLOW | unix.O_CLOEXEC
		if i < 3 {
			flags |= unix.O_DIRECTORY
		}
		next, e := unix.Openat(fd, part, flags, 0)
		unix.Close(fd)
		if e != nil {
			return nil, e
		}
		fd = next
		var st unix.Stat_t
		if e = unix.Fstat(fd, &st); e != nil {
			unix.Close(fd)
			return nil, e
		}
		kind := uint32(unix.S_IFDIR)
		if i == 3 {
			kind = unix.S_IFREG
		}
		if st.Uid != 0 || st.Mode&unix.S_IFMT != kind || st.Mode&07022 != 0 || (i == 3 && (st.Nlink != 1 || st.Mode&0111 == 0)) {
			unix.Close(fd)
			return nil, errors.New("unprotected installed requester")
		}
	}
	return os.NewFile(uintptr(fd), "installed-requester"), nil
}

func pdInstallRAM(data []byte) (*os.File, error) {
	directory, err := unix.Open("/dev/shm", unix.O_RDONLY|unix.O_DIRECTORY|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	defer unix.Close(directory)
	var fs unix.Statfs_t
	if err = unix.Fstatfs(directory, &fs); err != nil || fs.Type != unix.TMPFS_MAGIC {
		return nil, errors.New("installer configuration requires tmpfs")
	}
	var nonce [16]byte
	if _, err = rand.Read(nonce[:]); err != nil {
		return nil, err
	}
	name := ".aetherpd-config-" + hex.EncodeToString(nonce[:])
	writer, err := unix.Openat(directory, name, unix.O_WRONLY|unix.O_CREAT|unix.O_EXCL|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0600)
	if err != nil {
		return nil, err
	}
	defer unix.Close(writer)
	named := true
	defer func() {
		if named {
			unix.Unlinkat(directory, name, 0)
		}
	}()
	reader, err := unix.Openat(directory, name, unix.O_RDONLY|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	file := os.NewFile(uintptr(reader), "private-installer-config")
	ok := false
	defer func() {
		if !ok {
			file.Close()
		}
	}()
	var a, b unix.Stat_t
	if unix.Fstat(writer, &a) != nil || unix.Fstat(reader, &b) != nil || a.Dev != b.Dev || a.Ino != b.Ino {
		return nil, errors.New("configuration inode changed")
	}
	if err = unix.Unlinkat(directory, name, 0); err != nil {
		return nil, err
	}
	named = false
	// No credential byte is written until the name is gone.
	for len(data) > 0 {
		n, e := unix.Write(writer, data)
		if e != nil {
			return nil, e
		}
		if n <= 0 {
			return nil, io.ErrShortWrite
		}
		data = data[n:]
	}
	ok = true
	return file, nil
}

func pdConfigPeer(conn *net.UnixConn, expected uint32) bool {
	raw, err := conn.SyscallConn()
	if err != nil {
		return false
	}
	var peer *syscall.Ucred
	var e error
	err = raw.Control(func(fd uintptr) { peer, e = syscall.GetsockoptUcred(int(fd), syscall.SOL_SOCKET, syscall.SO_PEERCRED) })
	return err == nil && e == nil && peer != nil && peer.Uid == expected
}
func pdDiscardRights(control []byte) {
	messages, _ := syscall.ParseSocketControlMessage(control)
	for _, message := range messages {
		if message.Header.Level == syscall.SOL_SOCKET && message.Header.Type == syscall.SCM_RIGHTS {
			fds, _ := syscall.ParseUnixRights(&message)
			for _, fd := range fds {
				syscall.Close(fd)
			}
		}
	}
}
func servePDInstallConfig(conn *net.UnixConn, expectedUID uint32, public [120]byte, config *os.File) bool {
	if !pdConfigPeer(conn, expectedUID) {
		return false
	}
	if conn.SetDeadline(time.Now().Add(3*time.Second)) != nil {
		return false
	}
	var request [129]byte
	control := make([]byte, syscall.CmsgSpace(32))
	n, oob, flags, _, err := conn.ReadMsgUnix(request[:], control)
	pdDiscardRights(control[:oob])
	if err != nil || n != 128 || oob != 0 || flags&(syscall.MSG_TRUNC|syscall.MSG_CTRUNC) != 0 || string(request[:8]) != "PDCFG001" || !bytes.Equal(request[8:128], public[:]) {
		return false
	}
	n, oob, err = conn.WriteMsgUnix([]byte("PDSECR01"), syscall.UnixRights(int(config.Fd())), nil)
	return err == nil && n == 8 && oob == len(syscall.UnixRights(int(config.Fd())))
}
func pdInstallConfigBroker(ctx context.Context, public [120]byte, config *os.File) (func(), error) {
	address := "@aetherpd-config-" + hex.EncodeToString(public[88:])
	listener, err := net.ListenUnix("unixpacket", &net.UnixAddr{Name: address, Net: "unixpacket"})
	if err != nil {
		return nil, err
	}
	brokerCtx, cancel := context.WithCancel(ctx)
	done := make(chan struct{})
	closeOnCancel := context.AfterFunc(brokerCtx, func() { listener.Close() })
	go func() {
		defer close(done)
		defer listener.Close()
		for {
			conn, err := listener.AcceptUnix()
			if err != nil {
				return
			}
			stop := context.AfterFunc(brokerCtx, func() { conn.Close() })
			sent := servePDInstallConfig(conn, 0, public, config)
			stop()
			conn.Close()
			if sent {
				return
			}
		}
	}()
	return func() { cancel(); closeOnCancel(); listener.Close(); <-done }, nil
}

type pdInstallOutput struct {
	bytes.Buffer
	overflow bool
}

func (b *pdInstallOutput) Write(data []byte) (int, error) {
	n := len(data)
	space := 4096 - b.Len()
	if len(data) > space {
		data = data[:space]
		b.overflow = true
	}
	b.Buffer.Write(data)
	return n, nil
}
func runPDInstallRequest(ctx context.Context, config [120]byte, ready func()) (int, error) {
	defer clear(config[:])
	if err := pdInstallIdentity(); err != nil {
		return 6, err
	}
	program, err := pdInstallProgram()
	if err != nil {
		return 6, err
	}
	defer program.Close()
	return runPDInstallProgram(ctx, program, config, ready)
}
func runPDInstallProgram(ctx context.Context, program *os.File, config [120]byte, ready func()) (int, error) {
	defer clear(config[:])
	public, err := pdInstallPublic(config[:])
	if err != nil {
		return 6, err
	}
	private, err := pdInstallRAM(config[:])
	if err != nil {
		return 6, err
	}
	defer private.Close()
	closeBroker, err := pdInstallConfigBroker(ctx, public, private)
	if err != nil {
		return 6, err
	}
	defer closeBroker()
	readReady, writeReady, err := os.Pipe()
	if err != nil {
		return 6, err
	}
	defer readReady.Close()
	defer writeReady.Close()
	childCtx, cancel := context.WithCancel(ctx)
	defer cancel()
	cmd := exec.CommandContext(childCtx, "/proc/self/fd/5", "--install-request")
	cmd.Args[0] = "pd-primary-launch"
	cmd.Env = []string{"PATH=/usr/bin:/bin"}
	cmd.ExtraFiles = []*os.File{private, writeReady, program}
	cmd.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGKILL}
	var output, stderr pdInstallOutput
	cmd.Stdout = &output
	cmd.Stderr = &stderr
	cmd.WaitDelay = time.Second
	if err = cmd.Start(); err != nil {
		return 6, err
	}
	writeReady.Close()
	closeReady := context.AfterFunc(childCtx, func() { readReady.Close() })
	defer closeReady()
	// Native readiness precedes network admission. Cancellation closes this pipe
	// and kills the owned child; it cannot leave a detached installer listener.
	var byteReady [1]byte
	_, readErr := io.ReadFull(readReady, byteReady[:])
	readReady.Close()
	if readErr != nil || byteReady[0] != 'R' {
		cancel()
		cmd.Wait()
		return 6, errors.New("native requester did not become ready")
	}
	ready()
	waitErr := cmd.Wait()
	var result struct {
		InputsVerified        bool `json:"inputs_verified"`
		DeliveryResult        int  `json:"delivery_result"`
		InstallationCommitted bool `json:"installation_committed"`
	}
	if output.overflow || stderr.overflow || stderr.Len() != 0 || json.Unmarshal(output.Bytes(), &result) != nil || result.InstallationCommitted {
		return 6, errors.New("invalid requester result")
	}
	code := 0
	if waitErr != nil {
		var exit *exec.ExitError
		if !errors.As(waitErr, &exit) {
			return 6, waitErr
		}
		code = exit.ExitCode()
	}
	if code != result.DeliveryResult || (code != 0 && code != 3 && code != 4 && code != 5 && code != 6) || (code != 6 && !result.InputsVerified) {
		return 6, errors.New("requester result disagrees with exit")
	}
	if ctx.Err() != nil {
		return 6, ctx.Err()
	}
	return code, nil
}
