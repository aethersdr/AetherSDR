// The temporary ARM64 carrier accepts only the inherited descriptor contract
// from pd-accept. It has no network listener, command API or caller-supplied path.
package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"
)

const imageLabel = "aetherpd.carrier"
const requestLabel = "aetherpd.request"

type container struct {
	ID    string `json:"Id"`
	Image string
	State struct {
		Pid     int
		Running bool
	}
	Config struct {
		Entrypoint []string
		Labels     map[string]string
	}
}

// The radio's compatibility API ignores CgroupnsMode. Use the native creation
// schema so process attestation sees the host's absolute service paths.
type namespace struct {
	NSMode string `json:"nsmode"`
}
type createRequest struct {
	Name               string            `json:"name"`
	Image              string            `json:"image"`
	User               string            `json:"user"`
	Entrypoint         []string          `json:"entrypoint"`
	Command            []string          `json:"command"`
	Labels             map[string]string `json:"labels"`
	PidNS              namespace         `json:"pidns"`
	NetNS              namespace         `json:"netns"`
	CgroupNS           namespace         `json:"cgroupns"`
	Privileged         bool              `json:"privileged"`
	ReadOnlyFilesystem bool              `json:"read_only_filesystem"`
	CapDrop            []string          `json:"cap_drop"`
	CapAdd             []string          `json:"cap_add"`
	Remove             bool              `json:"remove"`
	NoNewPrivileges    bool              `json:"no_new_privileges"`
	RestartPolicy      string            `json:"restart_policy"`
}

func exactHex(s string, n int) bool {
	if len(s) != n {
		return false
	}
	for _, c := range s {
		if !(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

func fixedChild(image, nonce string) (createRequest, error) {
	if !exactHex(strings.TrimPrefix(image, "sha256:"), 64) || !exactHex(nonce, 32) {
		return createRequest{}, errors.New("invalid carrier identity")
	}
	r := createRequest{Name: "aetherpd-install-" + nonce, Image: image, User: "0:0", Entrypoint: []string{"/pd/deliver"}, Command: []string{"--listen-relay"},
		Labels: map[string]string{imageLabel: "v1", requestLabel: nonce},
		PidNS:  namespace{"host"}, NetNS: namespace{"host"}, CgroupNS: namespace{"host"},
		ReadOnlyFilesystem: true, Remove: true, NoNewPrivileges: true, RestartPolicy: "no",
		CapDrop: []string{"ALL"}, CapAdd: []string{"CHOWN", "DAC_OVERRIDE", "FOWNER", "SYS_CHROOT", "SYS_PTRACE"}}
	return r, nil
}

type api struct{ client *http.Client }

func (a api) request(ctx context.Context, method, path string, body any, out any) (int, error) {
	var encoded []byte
	var err error
	if body != nil {
		encoded, err = json.Marshal(body)
		if err != nil {
			return 0, err
		}
	}
	req, err := http.NewRequestWithContext(ctx, method, "http://localhost"+path, bytes.NewReader(encoded))
	if err != nil {
		return 0, err
	}
	req.Header.Set("Content-Type", "application/json")
	response, err := a.client.Do(req)
	if err != nil {
		return 0, err
	}
	defer response.Body.Close()
	data, err := io.ReadAll(io.LimitReader(response.Body, 65537))
	if err != nil || len(data) > 65536 {
		return response.StatusCode, errors.New("invalid carrier API response")
	}
	if response.StatusCode >= 300 {
		return response.StatusCode, fmt.Errorf("carrier API status %d", response.StatusCode)
	}
	if out != nil {
		if err = json.Unmarshal(data, out); err != nil {
			return response.StatusCode, errors.New("invalid carrier API JSON")
		}
	}
	return response.StatusCode, nil
}

func (a api) inspect(ctx context.Context, id string) (container, int, error) {
	var c container
	status, err := a.request(ctx, "GET", "/containers/"+id+"/json", nil, &c)
	return c, status, err
}

func logText(data []byte) string {
	var text strings.Builder
	for len(data) >= 8 {
		if (data[0] != 1 && data[0] != 2) || data[1] != 0 || data[2] != 0 || data[3] != 0 {
			break
		}
		size := uint64(binary.BigEndian.Uint32(data[4:8]))
		data = data[8:]
		if size > uint64(len(data)) {
			text.Write(data)
			break
		}
		text.Write(data[:size])
		data = data[size:]
	}
	return text.String()
}

// Start before releasing the installer's request; AutoRemove must not erase its
// failure diagnostics before the supervisor reads them. Logs never drive state.
func (a api) watchLogs(ctx context.Context, id string) func() {
	ctx, cancel := context.WithCancel(ctx)
	done := make(chan string, 1)
	go func() {
		client := *a.client
		client.Timeout = 0
		req, err := http.NewRequestWithContext(ctx, "GET", "http://localhost/containers/"+id+"/logs?stdout=true&stderr=true&follow=true", nil)
		if err != nil {
			done <- ""
			return
		}
		response, err := client.Do(req)
		if err != nil {
			done <- ""
			return
		}
		defer response.Body.Close()
		if response.StatusCode != 200 {
			done <- ""
			return
		}
		data, _ := io.ReadAll(io.LimitReader(response.Body, 4096))
		done <- logText(data)
	}()
	return func() {
		defer cancel()
		select {
		case text := <-done:
			if text != "" {
				_ = json.NewEncoder(os.Stderr).Encode(map[string]string{"installer_output": text})
			}
		case <-time.After(2 * time.Second):
			cancel()
		}
	}
}

func (a api) self(ctx context.Context, pid int) (container, error) {
	if pid <= 0 {
		return container{}, errors.New("invalid host PID")
	}
	var candidates []struct {
		ID string `json:"Id"`
	}
	_, err := a.request(ctx, "GET", "/containers/json?all=false&filters=%7B%22label%22%3A%5B%22aetherpd.carrier%3Dv1%22%5D%7D", nil, &candidates)
	if err != nil {
		return container{}, err
	}
	if len(candidates) > 16 {
		return container{}, errors.New("too many carrier candidates")
	}
	var found container
	for _, candidate := range candidates {
		if !exactHex(candidate.ID, 64) {
			return container{}, errors.New("invalid carrier candidate")
		}
		c, _, err := a.inspect(ctx, candidate.ID)
		if err != nil {
			return container{}, err
		}
		if c.State.Pid != pid {
			continue
		}
		if found.ID != "" || c.ID != candidate.ID || !c.State.Running || c.Config.Labels[imageLabel] != "v1" || len(c.Config.Entrypoint) != 1 || c.Config.Entrypoint[0] != "/pd/accept" {
			return container{}, errors.New("outer container does not match current process")
		}
		found = c
	}
	if found.ID == "" {
		return container{}, errors.New("outer container not found")
	}
	return found, nil
}

// Cleanup also covers a create whose HTTP reply was lost. The per-run name,
// random ownership label and pinned image must all agree before removal.
func (a api) cleanup(name, nonce, image string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	c, status, err := a.inspect(ctx, name)
	if status == 404 {
		return nil
	}
	if err != nil {
		return err
	}
	if !exactHex(c.ID, 64) || c.Image != image || c.Config.Labels[requestLabel] != nonce || c.Config.Labels[imageLabel] != "v1" {
		return errors.New("refusing foreign container cleanup")
	}
	status, err = a.request(ctx, "DELETE", "/containers/"+c.ID+"?force=true", nil, nil)
	if status == 404 {
		return nil
	}
	return err
}

func inherited() ([]*os.File, []byte, error) {
	if os.Getuid() != 0 || os.Geteuid() != 0 {
		return nil, nil, errors.New("carrier must be root with retired capabilities")
	}
	status, err := os.ReadFile("/proc/self/status")
	if err != nil {
		return nil, nil, err
	}
	seen := map[string]bool{}
	for _, line := range strings.Split(string(status), "\n") {
		fields := strings.Fields(line)
		if len(fields) != 2 {
			continue
		}
		switch fields[0] {
		case "CapEff:", "CapPrm:", "CapInh:", "CapBnd:", "CapAmb:":
			if fields[1] != "0000000000000000" {
				return nil, nil, errors.New("carrier capabilities not retired")
			}
			seen[fields[0]] = true
		case "NoNewPrivs:":
			if fields[1] != "1" {
				return nil, nil, errors.New("carrier privilege regain enabled")
			}
			seen[fields[0]] = true
		}
	}
	for _, key := range []string{"CapEff:", "CapPrm:", "CapInh:", "CapBnd:", "NoNewPrivs:"} {
		if !seen[key] {
			return nil, nil, errors.New("missing privilege evidence")
		}
	}
	files := make([]*os.File, 10)
	for _, fd := range []int{0, 3, 4, 5, 7, 8} {
		files[fd] = os.NewFile(uintptr(fd), "inherited")
		if files[fd] == nil {
			return nil, nil, errors.New("missing descriptor")
		}
	}
	peer, err := syscall.GetsockoptUcred(7, syscall.SOL_SOCKET, syscall.SO_PEERCRED)
	kind, typeErr := syscall.GetsockoptInt(7, syscall.SOL_SOCKET, syscall.SO_TYPE)
	address, addressErr := syscall.Getsockname(7)
	if _, unix := address.(*syscall.SockaddrUnix); err != nil || peer == nil || peer.Uid != 78666 || typeErr != nil || kind != syscall.SOCK_SEQPACKET || addressErr != nil || !unix {
		return nil, nil, errors.New("invalid original sender channel")
	}
	var st syscall.Stat_t
	if syscall.Fstat(3, &st) != nil || st.Uid != 0 || st.Mode&07777 != 0600 || st.Mode&syscall.S_IFMT != syscall.S_IFREG || st.Size != 120 || st.Nlink > 1 {
		return nil, nil, errors.New("invalid request configuration")
	}
	config := make([]byte, 120)
	n, err := files[3].ReadAt(config, 0)
	if err != nil || n != 120 || string(config[:8]) != "PDXFER01" || string(config[40:48]) != "PDDELIV1" || !bytes.Equal(config[48:52], []byte{0, 0, 0, 64}) {
		return nil, nil, errors.New("invalid request record")
	}
	if bytes.Equal(config[8:40], make([]byte, 32)) {
		return nil, nil, errors.New("empty request key")
	}
	role := config[55]
	if !bytes.Equal(config[52:55], []byte{0, 0, 0}) || (role != 1 && role != 2) {
		return nil, nil, errors.New("invalid request role")
	}
	if role == 2 {
		files[6] = os.NewFile(6, "previous")
		if syscall.Fstat(6, &st) != nil {
			return nil, nil, errors.New("missing predecessor")
		}
	}
	if syscall.Fstat(8, &st) != nil || st.Mode&syscall.S_IFMT != syscall.S_IFDIR || st.Uid != 0 || st.Mode&0022 != 0 {
		return nil, nil, errors.New("invalid host root")
	}
	var fs syscall.Statfs_t
	if syscall.Statfs("/proc/self/fd/8/proc", &fs) != nil || fs.Type != 0x9fa0 {
		return nil, nil, errors.New("invalid host procfs")
	}
	return files, config, nil
}

func connectRelay(ctx context.Context, receipt []byte) (*os.File, error) {
	name := "@aetherpd-relay-" + hex.EncodeToString(receipt[16:48])
	for {
		conn, err := (&net.Dialer{}).DialContext(ctx, "unixpacket", name)
		if err == nil {
			defer conn.Close()
			return conn.(*net.UnixConn).File()
		}
		if !errors.Is(err, syscall.ECONNREFUSED) && !errors.Is(err, syscall.ENOENT) {
			return nil, err
		}
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(50 * time.Millisecond):
		}
	}
}

type boundedLog struct {
	bytes.Buffer
	overflow bool
}

func (b *boundedLog) Write(p []byte) (int, error) {
	n := len(p)
	space := 4096 - b.Len()
	if len(p) > space {
		p = p[:space]
		b.overflow = true
	}
	_, _ = b.Buffer.Write(p)
	return n, nil
}

func run(ctx context.Context) (result int, cleanupOK bool, err error) {
	files, config, err := inherited()
	if err != nil {
		return 6, false, err
	}
	defer clear(config)
	defer func() {
		for _, file := range files {
			if file != nil {
				file.Close()
			}
		}
	}()
	defer func() {
		delivered := result
		if err != nil {
			delivered = 4
		}
		if !cleanupOK {
			delivered = 5
		}
		if !sendCompletion(int(files[7].Fd()), delivered) {
			err = errors.Join(err, errors.New("original sender completion unavailable"))
		}
	}()
	transport := &http.Transport{DisableKeepAlives: true, DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
		return (&net.Dialer{}).DialContext(ctx, "unix", "/proc/self/fd/8/run/podman/podman.sock")
	}}
	defer transport.CloseIdleConnections()
	a := api{&http.Client{Transport: transport, Timeout: 5 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return errors.New("redirect refused") }}}
	pidText, err := os.Readlink("/proc/self/fd/8/proc/self")
	if err != nil {
		return 6, false, err
	}
	pid, err := strconv.Atoi(pidText)
	if err != nil {
		return 6, false, errors.New("invalid host PID")
	}
	self, err := a.self(ctx, pid)
	if err != nil {
		return 6, false, err
	}
	var random [16]byte
	if _, err = rand.Read(random[:]); err != nil {
		return 6, false, err
	}
	nonce := hex.EncodeToString(random[:])
	child, err := fixedChild(self.Image, nonce)
	if err != nil {
		return 6, false, err
	}
	name := "aetherpd-install-" + nonce
	defer func() {
		cleanupErr := a.cleanup(name, nonce, self.Image)
		cleanupOK = cleanupErr == nil
		if cleanupErr != nil {
			err = errors.Join(err, cleanupErr)
		}
	}()
	var created struct {
		ID string `json:"Id"`
	}
	if _, err = a.request(ctx, "POST", "/v3.0.0/libpod/containers/create", child, &created); err != nil {
		return 6, false, err
	}
	if !exactHex(created.ID, 64) {
		return 6, false, errors.New("invalid created container identity")
	}
	if _, err = a.request(ctx, "POST", "/containers/"+created.ID+"/start", nil, nil); err != nil {
		return 6, false, err
	}
	finishLogs := a.watchLogs(ctx, created.ID)
	defer finishLogs()
	connectCtx, cancel := context.WithTimeout(ctx, 8*time.Second)
	channel, err := connectRelay(connectCtx, config[40:])
	cancel()
	if err != nil {
		return 6, false, err
	}
	defer channel.Close()
	// ExtraFiles fixes the child ABI at descriptors 3 through 9. Missing slots
	// are closed by os/exec, not filled with caller-selected files.
	cmd := exec.CommandContext(ctx, "/pd/send", "--relay")
	cmd.Env = []string{"PATH=/usr/bin:/bin"}
	cmd.Stdin = files[0]
	cmd.ExtraFiles = []*os.File{files[3], files[4], files[5], files[6], nil, files[8], channel}
	cmd.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGKILL}
	var stdout, stderr boundedLog
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	cmd.WaitDelay = 2 * time.Second
	execErr := cmd.Run()
	var completion struct {
		Sent      bool `json:"descriptors_sent"`
		Completed bool
		Result    int `json:"delivery_result"`
	}
	if stdout.overflow || stderr.overflow || stderr.Len() != 0 || json.Unmarshal(stdout.Bytes(), &completion) != nil || !completion.Sent || !completion.Completed {
		return 6, false, errors.New("missing authenticated delivery completion")
	}
	if completion.Result != 0 && completion.Result != 3 && completion.Result != 4 && completion.Result != 5 {
		return 6, false, errors.New("invalid delivery result")
	}
	code := 0
	if execErr != nil {
		var exit *exec.ExitError
		if !errors.As(execErr, &exit) {
			return 6, false, execErr
		}
		code = exit.ExitCode()
	}
	if code != completion.Result {
		return 6, false, errors.New("delivery result disagrees with exit status")
	}
	return completion.Result, false, nil
}

// The retained socket was authenticated by pd-accept. Completion is a bounded
// packet on that connection, never inferred from child logs or container exit.
func sendCompletion(fd, result int) bool {
	if result != 0 && result != 3 && result != 4 && result != 5 {
		return false
	}
	bytes := make([]byte, 12)
	copy(bytes, "PDDONE01")
	binary.BigEndian.PutUint32(bytes[8:], uint32(result))
	return syscall.Sendto(fd, bytes, syscall.MSG_NOSIGNAL|syscall.MSG_DONTWAIT, nil) == nil
}

func main() {
	if len(os.Args) != 1 {
		os.Exit(2)
	}
	parent, cancel := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT, syscall.SIGHUP)
	defer cancel()
	ctx, deadline := context.WithTimeout(parent, 120*time.Second)
	defer deadline()
	result, clean, err := run(ctx)
	// Errors deliberately exclude response bodies, configuration and secrets.
	if err != nil {
		fmt.Fprintln(os.Stderr, "carrier:", err)
	}
	_ = json.NewEncoder(os.Stdout).Encode(map[string]any{"delivery_result": result, "child_removed": clean, "rf_commands": []string{}})
	if err != nil || !clean {
		os.Exit(6)
	}
	os.Exit(result)
}
