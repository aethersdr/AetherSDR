//go:build linux

package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"golang.org/x/crypto/blake2b"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
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
				done <- servePDInstallConfig(conn, uid, public, file, 3) != 0
			}()
			conn, err := net.DialUnix("unixpacket", nil, listener.Addr().(*net.UnixAddr))
			if err != nil {
				t.Fatal(err)
			}
			defer conn.Close()
			conn.SetDeadline(time.Now().Add(time.Second))
			frame := append([]byte("PDCFGAC1"), public[:]...)
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
				if (!errors.Is(readErr, io.EOF) && !errors.Is(readErr, syscall.ECONNRESET)) || n > 0 || o != 0 {
					t.Fatalf("refused peer read: bytes=%d ancillary=%d error=%v", n, o, readErr)
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
	conn.Write(append([]byte("PDCFGAC1"), public[:]...))
	var data [16]byte
	oob := make([]byte, 64)
	n, o, _, _, err := conn.ReadMsgUnix(data[:], oob)
	pdDiscardRights(oob[:o])
	conn.Close()
	if (!errors.Is(err, io.EOF) && !errors.Is(err, syscall.ECONNRESET)) || n > 0 || o != 0 {
		t.Fatalf("nonroot read: bytes=%d ancillary=%d error=%v", n, o, err)
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

func TestPDInstallNativeRejectsPersistentOrWritableCredentials(t *testing.T) {
	program := pdNativeRequestFixture(t)
	config := pdInstallFixture()
	for _, scenario := range []string{"named-ram", "disk", "writable", "permissions", "wrong-size", "not-a-ready-pipe"} {
		t.Run(scenario, func(t *testing.T) {
			var file *os.File
			var err error
			if scenario == "named-ram" || scenario == "disk" || scenario == "writable" {
				directory := "/dev/shm"
				if scenario == "disk" {
					directory = os.Getenv("AETHERPD_TEST_DISK_DIRECTORY")
					if directory == "" {
						t.Skip("set AETHERPD_TEST_DISK_DIRECTORY to a non-tmpfs directory")
					}
				}
				writer, e := os.CreateTemp(directory, "pd-private-config-test-")
				if e != nil {
					t.Fatal(e)
				}
				defer writer.Close()
				defer os.Remove(writer.Name())
				if _, e = writer.Write(config[:]); e != nil {
					t.Fatal(e)
				}
				if scenario == "writable" {
					file = writer
				} else {
					file, err = os.Open(writer.Name())
					if err != nil {
						t.Fatal(err)
					}
				}
				if scenario != "named-ram" {
					if e = os.Remove(writer.Name()); e != nil {
						t.Fatal(e)
					}
				}
				if scenario == "disk" {
					var fs unix.Statfs_t
					if unix.Fstatfs(int(file.Fd()), &fs) != nil || fs.Type == unix.TMPFS_MAGIC {
						t.Fatal("disk refusal fixture must actually use disk")
					}
				}
			} else {
				file, err = pdInstallRAM(config[:])
				if err != nil {
					t.Fatal(err)
				}
			}
			defer file.Close()
			if scenario == "permissions" {
				if err = file.Chmod(0644); err != nil {
					t.Fatal(err)
				}
			}
			if scenario == "wrong-size" {
				writer, e := os.OpenFile(fmt.Sprintf("/proc/self/fd/%d", file.Fd()), os.O_WRONLY, 0)
				if e != nil {
					t.Fatal(e)
				}
				e = writer.Truncate(119)
				writer.Close()
				if e != nil {
					t.Fatal(e)
				}
			}
			readReady, writeReady, err := os.Pipe()
			if err != nil {
				t.Fatal(err)
			}
			defer readReady.Close()
			defer writeReady.Close()
			inheritedReady := writeReady
			if scenario == "not-a-ready-pipe" {
				inheritedReady = file
			}
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			defer cancel()
			cmd := exec.CommandContext(ctx, "/proc/self/fd/5", "--install-request")
			cmd.Env = []string{"PATH=/usr/bin:/bin"}
			cmd.ExtraFiles = []*os.File{file, inheritedReady, program}
			output, runErr := cmd.CombinedOutput()
			var exit *exec.ExitError
			if !errors.As(runErr, &exit) || exit.ExitCode() != 3 || ctx.Err() != nil {
				t.Fatalf("unsafe credential accepted or rejected for the wrong reason: %v, %s", runErr, output)
			}
		})
	}
}

func pdNativeRequestFixture(t *testing.T) *os.File {
	t.Helper()
	path := os.Getenv("AETHERPD_REQUEST_FIXTURE")
	if path == "" {
		t.Skip("set AETHERPD_REQUEST_FIXTURE to the built --inherited-fixture executable")
	}
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { file.Close() })
	return file
}
func pdRequestNativeCarrier(listener *net.UnixListener, config [120]byte, inputs [][]byte) error {
	listener.SetDeadline(time.Now().Add(8 * time.Second))
	conn, err := listener.AcceptUnix()
	if err != nil {
		return err
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(5 * time.Second))
	nonce := bytes.Repeat([]byte{'n'}, 32)
	mac := func(domain string) []byte {
		h, _ := blake2b.New256(config[8:40])
		h.Write([]byte(domain))
		h.Write(nonce)
		h.Write(config[40:])
		return h.Sum(nil)
	}
	hello := append(append([]byte("PDCARR01"), nonce...), mac("PDSERV01")...)
	if _, err = conn.Write(hello); err != nil {
		return err
	}
	frame := make([]byte, 64)
	control := make([]byte, syscall.CmsgSpace(32))
	n, o, flags, _, err := conn.ReadMsgUnix(frame, control)
	fds := []int{}
	messages, _ := syscall.ParseSocketControlMessage(control[:o])
	for _, message := range messages {
		rights, _ := syscall.ParseUnixRights(&message)
		fds = append(fds, rights...)
	}
	defer func() {
		for _, fd := range fds {
			syscall.Close(fd)
		}
	}()
	if err != nil || n != 40 || flags & ^syscall.MSG_CMSG_CLOEXEC != 0 || len(fds) != 5 || !bytes.Equal(frame[:n], append([]byte("PDROOT01"), mac("PDSEND01")...)) {
		return fmt.Errorf("native descriptor authentication failed: n=%d fds=%d flags=%d error=%v", n, len(fds), flags, err)
	}
	for i, fd := range fds {
		var st unix.Stat_t
		if unix.Fstat(fd, &st) != nil {
			return errors.New("received fd unavailable")
		}
		if i == 0 {
			if st.Mode&unix.S_IFMT != unix.S_IFDIR || st.Uid != 0 {
				return errors.New("invalid root descriptor")
			}
			continue
		}
		content := make([]byte, len(inputs[i-1])+1)
		n, e := unix.Pread(fd, content, 0)
		mode, e2 := unix.FcntlInt(uintptr(fd), unix.F_GETFL, 0)
		if e != nil || e2 != nil || mode&unix.O_ACCMODE != unix.O_RDONLY || st.Nlink != 0 || !bytes.Equal(content[:n], inputs[i-1]) {
			return errors.New("native input descriptor not exact, readonly and unlinked")
		}
	}
	_, err = conn.Write(append([]byte("PDDONE01"), 0, 0, 0, 3))
	return err
}
func TestPDInstallNativeUploadAndCancellation(t *testing.T) {
	program := pdNativeRequestFixture(t)
	for _, cancelOnly := range []bool{false, true} {
		t.Run(map[bool]string{false: "authenticated-delivery", true: "cancel-before-upload"}[cancelOnly], func(t *testing.T) {
			n := newTestNode(t, "owner")
			inputs := [][]byte{bytes.Repeat([]byte{'e'}, 185), bytes.Repeat([]byte{'p'}, 352), bytes.Repeat([]byte{'c'}, 432), bytes.Repeat([]byte{'o'}, 432)}
			config := pdInstallFixture()
			envelope := blake2b.Sum256(inputs[0])
			plan := blake2b.Sum256(inputs[1])
			copy(config[56:88], envelope[:])
			copy(config[88:], plan[:])
			if cancelOnly {
				config[8] ^= 1
			}
			n.pdInstallRun = func(ctx context.Context, private [120]byte, ready func()) (int, error) {
				return runPDInstallProgram(ctx, program, private, ready)
			}
			h := NewAPI(n)
			w := pdInstallCall(h, "POST", "/v1/aetherpd/install-request", "owner", config[:])
			if w.Code != 202 {
				t.Fatal(w.Code, w.Body.String())
			}
			n.mu.Lock()
			task := n.pdInstall
			n.mu.Unlock()
			t.Cleanup(func() { task.cancel(); pdInstallWait(t, n) })
			path := "/v1/aetherpd/install-request/" + task.id()
			if cancelOnly {
				if w := pdInstallCall(h, "DELETE", path, "owner", nil); w.Code != 202 {
					t.Fatal(w.Code)
				}
				pdInstallWait(t, n)
				if conn, err := net.DialTimeout("tcp", "127.0.0.1:48995", time.Second); err == nil {
					conn.Close()
					t.Fatal("cancelled native listener survived")
				}
				return
			}
			listener, err := net.ListenUnix("unixpacket", &net.UnixAddr{Name: "@aetherpd-bootstrap-" + hex.EncodeToString(envelope[:]), Net: "unixpacket"})
			if err != nil {
				t.Fatal(err)
			}
			defer listener.Close()
			delivered := make(chan error, 1)
			go func() { delivered <- pdRequestNativeCarrier(listener, config, inputs) }()
			script := `import sys,json,socket
sys.path.insert(0,'../pd-waveform/request')
import client
request=json.load(sys.stdin)
with socket.create_connection(('127.0.0.1',48995),3) as channel:
 upload=client.Upload(channel,bytes.fromhex(request['config']))
 first=upload.send([bytes.fromhex(x) for x in request['inputs']])
 second=upload.finish()
 assert first=={'inputs_verified':True,'installation_committed':False}
 assert second=={'delivery_result':3,'installation_committed':False},second
 print('authenticated native delivery refusal received')
`
			encoded := []string{}
			for _, data := range inputs {
				encoded = append(encoded, hex.EncodeToString(data))
			}
			body, _ := json.Marshal(map[string]any{"config": hex.EncodeToString(config[:]), "inputs": encoded})
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			command := exec.CommandContext(ctx, "python3", "-c", script)
			command.Stdin = bytes.NewReader(body)
			output, err := command.CombinedOutput()
			clear(body)
			if err != nil {
				t.Fatal(err, string(output), <-delivered)
			}
			if err = <-delivered; err != nil {
				t.Fatal(err)
			}
			pdInstallWait(t, n)
			w = pdInstallCall(h, "GET", path, "owner", nil)
			if w.Code != 200 || !strings.Contains(w.Body.String(), `"delivery_result":3`) || !strings.Contains(w.Body.String(), `"state":"delivery_finished"`) || !strings.Contains(w.Body.String(), `"installation_committed":false`) {
				t.Fatal(w.Code, w.Body.String())
			}
			// The public artifact commits to the request without carrying the secret.
			public, err := pdInstallPublic(config[:])
			if err != nil {
				t.Fatal(err)
			}
			if binary.BigEndian.Uint32(public[16:20]) != 64 || bytes.Contains(public[:], config[8:40]) {
				t.Fatal("bad public request record")
			}
		})
	}
}

func pdRuntimeConfigFixture(t *testing.T) string {
	t.Helper()
	path := os.Getenv("AETHERPD_CONFIG_FIXTURE")
	if path == "" {
		t.Skip("set AETHERPD_CONFIG_FIXTURE to the native runtime configuration checker")
	}
	return path
}
func pdRuntimeFetch(t *testing.T, checker, path, role string, expected int) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 6*time.Second)
	defer cancel()
	output, err := exec.CommandContext(ctx, checker, path, role).CombinedOutput()
	code := 0
	if err != nil {
		var exit *exec.ExitError
		if !errors.As(err, &exit) {
			t.Fatal(err)
		}
		code = exit.ExitCode()
	}
	if ctx.Err() != nil || code != expected || !bytes.Contains(output, []byte(`"no_descriptor_leak":true`)) {
		t.Fatalf("native fetch: exit=%d wanted=%d error=%v output=%s", code, expected, err, output)
	}
	if expected == 0 && !bytes.Contains(output, []byte(`"credential_verified":true`)) {
		t.Fatal(string(output))
	}
}
func TestPDInstallNativeCredentialRolesAndBinding(t *testing.T) {
	checker := pdRuntimeConfigFixture(t)
	for _, first := range []string{"accept", "deliver"} {
		t.Run(first+"-first", func(t *testing.T) {
			config := pdInstallFixture()
			public, _ := pdInstallPublic(config[:])
			file, err := pdInstallRAM(config[:])
			if err != nil {
				t.Fatal(err)
			}
			defer file.Close()
			stop, err := pdInstallConfigBrokerForUID(context.Background(), public, file, uint32(os.Getuid()))
			if err != nil {
				t.Fatal(err)
			}
			defer stop()
			path := filepath.Join(t.TempDir(), "request")
			for _, scenario := range []string{"magic", "receipt", "commitment", "zero-commitment", "permissions"} {
				bad := public
				mode := os.FileMode(0444)
				switch scenario {
				case "magic":
					bad[0] ^= 1
				case "receipt":
					bad[24] ^= 1
				case "commitment":
					bad[100] ^= 1
				case "zero-commitment":
					clear(bad[88:])
				case "permissions":
					mode = 0666
				}
				os.Remove(path)
				if err = os.WriteFile(path, bad[:], 0600); err != nil {
					t.Fatal(err)
				}
				if err = os.Chmod(path, mode); err != nil {
					t.Fatal(err)
				}
				pdRuntimeFetch(t, checker, path, first, 3)
			}
			os.Remove(path)
			if err = os.WriteFile(path, public[:], 0444); err != nil {
				t.Fatal(err)
			}
			pdRuntimeFetch(t, checker, path, "arbitrary-role", 3)
			pdRuntimeFetch(t, checker, path, first, 0)
			pdRuntimeFetch(t, checker, path, first, 3)
			second := "deliver"
			if first == second {
				second = "accept"
			}
			pdRuntimeFetch(t, checker, path, second, 0)
			pdRuntimeFetch(t, checker, path, second, 3)
		})
	}
}
func TestPDInstallNativeCredentialRejectsBadDescriptors(t *testing.T) {
	checker := pdRuntimeConfigFixture(t)
	for _, scenario := range []string{"wrong-key", "wrong-receipt", "writable", "named", "mode", "extra-fd", "wrong-reply", "truncated-control"} {
		t.Run(scenario, func(t *testing.T) {
			config := pdInstallFixture()
			public, _ := pdInstallPublic(config[:])
			content := config
			if scenario == "wrong-key" {
				content[8] ^= 1
			}
			if scenario == "wrong-receipt" {
				content[56] ^= 1
			}
			file, err := pdInstallRAM(content[:])
			if err != nil {
				t.Fatal(err)
			}
			defer file.Close()
			if scenario == "mode" {
				if err = file.Chmod(0644); err != nil {
					t.Fatal(err)
				}
			}
			if scenario == "named" {
				named, e := os.CreateTemp("/dev/shm", "pd-credential-negative-")
				if e != nil {
					t.Fatal(e)
				}
				defer named.Close()
				defer os.Remove(named.Name())
				named.Write(content[:])
				file, e = os.Open(named.Name())
				if e != nil {
					t.Fatal(e)
				}
				defer file.Close()
			}
			if scenario == "writable" {
				writer, e := os.OpenFile(fmt.Sprintf("/proc/self/fd/%d", file.Fd()), os.O_RDWR, 0)
				if e != nil {
					t.Fatal(e)
				}
				defer writer.Close()
				file = writer
			}
			path := filepath.Join(t.TempDir(), "request")
			if err = os.WriteFile(path, public[:], 0444); err != nil {
				t.Fatal(err)
			}
			listener, err := net.ListenUnix("unixpacket", &net.UnixAddr{Name: "@aetherpd-config-" + hex.EncodeToString(public[88:]), Net: "unixpacket"})
			if err != nil {
				t.Fatal(err)
			}
			defer listener.Close()
			done := make(chan error, 1)
			go func() {
				listener.SetDeadline(time.Now().Add(5 * time.Second))
				conn, e := listener.AcceptUnix()
				if e != nil {
					done <- e
					return
				}
				defer conn.Close()
				conn.SetDeadline(time.Now().Add(3 * time.Second))
				request := make([]byte, 128)
				n, e := conn.Read(request)
				if e != nil || n != 128 {
					done <- fmt.Errorf("config request: n=%d error=%v", n, e)
					return
				}
				fds := []int{int(file.Fd())}
				reply := "PDSECR01"
				switch scenario {
				case "extra-fd":
					fds = append(fds, fds[0])
				case "wrong-reply":
					reply = "WRONG001"
				case "truncated-control":
					for len(fds) < 16 {
						fds = append(fds, fds[0])
					}
				}
				_, _, e = conn.WriteMsgUnix([]byte(reply), syscall.UnixRights(fds...), nil)
				done <- e
			}()
			pdRuntimeFetch(t, checker, path, "accept", 3)
			if err = <-done; err != nil {
				t.Fatal(err)
			}
		})
	}
}
