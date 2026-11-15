package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"syscall"
	"testing"
)

type roundTrip func(*http.Request) (*http.Response, error)

func (f roundTrip) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }
func response(status int, body string) *http.Response {
	return &http.Response{StatusCode: status, Body: io.NopCloser(strings.NewReader(body))}
}

func TestFixedChildAuthority(t *testing.T) {
	c, err := fixedChild(strings.Repeat("a", 64), strings.Repeat("b", 32))
	if err != nil {
		t.Fatal(err)
	}
	raw, _ := json.Marshal(c)
	var fields map[string]json.RawMessage
	if err = json.Unmarshal(raw, &fields); err != nil {
		t.Fatal(err)
	}
	expected := map[string]string{
		"name":  `"aetherpd-install-` + strings.Repeat("b", 32) + `"`,
		"image": `"` + strings.Repeat("a", 64) + `"`,
		"user":  `"0:0"`, "entrypoint": `["/pd/deliver"]`, "command": `["--listen-relay"]`,
		"pidns": `{"nsmode":"host"}`, "netns": `{"nsmode":"host"}`, "cgroupns": `{"nsmode":"host"}`,
		"privileged": "false", "read_only_filesystem": "true", "remove": "true",
		"cap_drop": `["ALL"]`, "cap_add": `["CHOWN","DAC_OVERRIDE","FOWNER","SYS_CHROOT","SYS_PTRACE"]`,
		"no_new_privileges": "true", "restart_policy": `"no"`,
	}
	if len(fields) != len(expected)+1 {
		t.Fatalf("unexpected execution contract %s", raw)
	}
	for key, value := range expected {
		if string(fields[key]) != value {
			t.Fatalf("unexpected %s: %s", key, raw)
		}
	}
	var labels map[string]string
	if json.Unmarshal(fields["labels"], &labels) != nil || len(labels) != 2 || labels[imageLabel] != "v1" || labels[requestLabel] != strings.Repeat("b", 32) {
		t.Fatal("invalid ownership labels")
	}
	for _, image := range []string{"latest", "../foreign", strings.Repeat("A", 64), strings.Repeat("a", 63)} {
		if _, err = fixedChild(image, strings.Repeat("b", 32)); err == nil {
			t.Fatalf("accepted %q", image)
		}
	}
}

func TestCleanupRequiresCompleteOwnership(t *testing.T) {
	image := strings.Repeat("a", 64)
	nonce := strings.Repeat("b", 32)
	id := strings.Repeat("c", 64)
	for _, wrong := range []string{"", "image", "nonce", "label", "id"} {
		t.Run(wrong, func(t *testing.T) {
			c := container{ID: id, Image: image}
			c.Config.Labels = map[string]string{imageLabel: "v1", requestLabel: nonce}
			switch wrong {
			case "image":
				c.Image = strings.Repeat("d", 64)
			case "nonce":
				c.Config.Labels[requestLabel] = "other"
			case "label":
				delete(c.Config.Labels, imageLabel)
			case "id":
				c.ID = "../foreign"
			}
			deleted := false
			a := api{&http.Client{Transport: roundTrip(func(r *http.Request) (*http.Response, error) {
				if r.Method == "GET" {
					b, _ := json.Marshal(c)
					return response(200, string(b)), nil
				}
				if r.Method != "DELETE" || r.URL.Path != "/containers/"+id || r.URL.RawQuery != "force=true" {
					t.Fatalf("unexpected deletion %s", r.URL)
				}
				deleted = true
				return response(204, ""), nil
			})}}
			err := a.cleanup("aetherpd-install-"+nonce, nonce, image)
			if wrong == "" && (err != nil || !deleted) {
				t.Fatalf("owned cleanup failed: %v", err)
			}
			if wrong != "" && (err == nil || deleted) {
				t.Fatalf("foreign cleanup accepted: %v", err)
			}
		})
	}
}

func TestCleanupAbsentAndResponseBounds(t *testing.T) {
	a := api{&http.Client{Transport: roundTrip(func(r *http.Request) (*http.Response, error) { return response(404, ""), nil })}}
	if err := a.cleanup("owned", "nonce", "image"); err != nil {
		t.Fatal(err)
	}
	a.client.Transport = roundTrip(func(r *http.Request) (*http.Response, error) { return response(200, strings.Repeat("x", 65537)), nil })
	if _, err := a.request(context.Background(), "GET", "/containers/owned/json", nil, nil); err == nil {
		t.Fatal("unbounded response accepted")
	}
}

func TestBoundedChildOutput(t *testing.T) {
	var b boundedLog
	n, err := b.Write([]byte(strings.Repeat("x", 8192)))
	if err != nil || n != 8192 || !b.overflow || b.Len() != 4096 {
		t.Fatal("child output not bounded")
	}
}

func TestInstallerLogsAreBoundedFramesNotControlInput(t *testing.T) {
	frame := func(stream byte, text string) []byte {
		b := make([]byte, 8)
		b[0] = stream
		binary.BigEndian.PutUint32(b[4:], uint32(len(text)))
		return append(b, []byte(text)...)
	}
	data := append(frame(1, "stdout\n"), frame(2, "stderr\n")...)
	if logText(data) != "stdout\nstderr\n" {
		t.Fatal("lost installer output")
	}
	if logText(frame(9, "foreign")) != "" || logText([]byte("short")) != "" {
		t.Fatal("accepted invalid framing")
	}
	truncated := frame(2, "bounded")
	binary.BigEndian.PutUint32(truncated[4:], 0xffffffff)
	if logText(truncated) != "bounded" {
		t.Fatal("unbounded frame length")
	}
}

func TestSelfRequiresMatchingLiveHostProcess(t *testing.T) {
	for _, fault := range []string{"", "pid", "entry", "label", "duplicate", "identity", "stopped"} {
		t.Run(fault, func(t *testing.T) {
			id := strings.Repeat("a", 64)
			c := container{ID: id, Image: strings.Repeat("b", 64)}
			c.State.Pid = 123
			c.State.Running = true
			c.Config.Entrypoint = []string{"/pd/accept"}
			c.Config.Labels = map[string]string{imageLabel: "v1"}
			candidates := []map[string]string{{"Id": id}}
			switch fault {
			case "pid":
				c.State.Pid = 456
			case "entry":
				c.Config.Entrypoint = []string{"/bin/sh"}
			case "label":
				c.Config.Labels = nil
			case "duplicate":
				candidates = append(candidates, candidates[0])
			case "identity":
				c.ID = strings.Repeat("c", 64)
			case "stopped":
				c.State.Running = false
			}
			a := api{&http.Client{Transport: roundTrip(func(r *http.Request) (*http.Response, error) {
				if r.Method != "GET" {
					t.Fatal("self lookup mutated state")
				}
				var body []byte
				if r.URL.Path == "/containers/json" {
					body, _ = json.Marshal(candidates)
				} else if r.URL.Path == "/containers/"+id+"/json" {
					body, _ = json.Marshal(c)
				} else {
					t.Fatal("unexpected self lookup")
				}
				return response(200, string(body)), nil
			})}}
			_, err := a.self(context.Background(), 123)
			if (err != nil) != (fault != "") {
				t.Fatalf("self lookup: %v", err)
			}
		})
	}
}

func TestOriginalSenderCompletion(t *testing.T) {
	for _, result := range []int{0, 3, 4, 5, 6} {
		pair, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_SEQPACKET|syscall.SOCK_CLOEXEC, 0)
		if err != nil {
			t.Fatal(err)
		}
		sent := sendCompletion(pair[0], result)
		buffer := make([]byte, 64)
		n, _, readErr := syscall.Recvfrom(pair[1], buffer, syscall.MSG_DONTWAIT)
		_ = syscall.Close(pair[0])
		_ = syscall.Close(pair[1])
		if result == 6 {
			if sent || readErr != syscall.EAGAIN {
				t.Fatal("accepted unknown completion")
			}
		} else if !sent || readErr != nil || n != 12 || string(buffer[:8]) != "PDDONE01" || binary.BigEndian.Uint32(buffer[8:12]) != uint32(result) {
			t.Fatal("lost or malformed completion")
		}
	}
	pair, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_SEQPACKET|syscall.SOCK_CLOEXEC, 0)
	if err != nil {
		t.Fatal(err)
	}
	_ = syscall.Close(pair[1])
	defer syscall.Close(pair[0])
	if sendCompletion(pair[0], 0) {
		t.Fatal("reported completion to disconnected sender")
	}
}
