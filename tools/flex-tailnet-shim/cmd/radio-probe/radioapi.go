package main

import (
	"bufio"
	"context"
	"fmt"
	"net"
	"strings"
	"sync"
	"time"
)

// RadioAPITest is one connection to the radio's API (TCP 4992) at one local
// address, and the VITA-49 the radio then sent this client.
type RadioAPITest struct {
	Addr        string    `json:"addr"`
	Connected   bool      `json:"connected"`
	Error       string    `json:"error,omitempty"`
	VersionLine string    `json:"version_line,omitempty"`
	HandleSeen  bool      `json:"handle_seen"`
	Replies     []string  `json:"replies,omitempty"`
	VITA        *VITATest `json:"vita,omitempty"`
}

// VITATest records where the radio's UDP came from. The shim must accept
// exactly that source, which on the 8600's container is 172.30.1.1:4993.
type VITATest struct {
	LocalPort  int      `json:"local_port"`
	Packets    int      `json:"packets"`
	FirstFrom  string   `json:"first_from,omitempty"`
	Sources    []string `json:"sources,omitempty"`
	FirstBytes string   `json:"first_bytes_hex,omitempty"`
	Error      string   `json:"error,omitempty"`
}

const apiPort = "4992"

// radioAPITests connects at loopback and at each LAN address: which one the
// radio answers, and the source of its VITA-49 to that client, decide how a
// native shim reaches the radio. The only commands are the two FlexLib's own
// connect sends to get meter data (Radio.cs: `client udpport`, `sub meter
// all`); nothing here can key the transmitter.
func radioAPITests(ctx context.Context, local []string) []RadioAPITest {
	addrs := append([]string{"127.0.0.1"}, local...)
	var out []RadioAPITest
	for _, a := range addrs {
		out = append(out, radioAPITest(ctx, a, 6*time.Second))
	}
	return out
}

func radioAPITest(ctx context.Context, host string, listenFor time.Duration) RadioAPITest {
	t := RadioAPITest{Addr: net.JoinHostPort(host, apiPort)}
	c, err := net.DialTimeout("tcp", t.Addr, 3*time.Second)
	if err != nil {
		t.Error = err.Error()
		return t
	}
	defer c.Close()
	t.Connected = true

	udp, err := net.ListenUDP("udp4", &net.UDPAddr{})
	if err != nil {
		t.Error = err.Error()
		return t
	}
	defer udp.Close()
	v := &VITATest{LocalPort: udp.LocalAddr().(*net.UDPAddr).Port}
	t.VITA = v

	var mu sync.Mutex
	readerDone := make(chan struct{})
	go func() {
		defer close(readerDone)
		srcs := map[string]bool{}
		buf := make([]byte, 2048)
		for {
			k, from, err := udp.ReadFromUDP(buf)
			if err != nil {
				return
			}
			mu.Lock()
			v.Packets++
			if v.FirstFrom == "" {
				v.FirstFrom = from.String()
				v.FirstBytes = fmt.Sprintf("%x", buf[:min(k, 8)])
			}
			if !srcs[from.String()] && len(srcs) < 8 {
				srcs[from.String()] = true
				v.Sources = append(v.Sources, from.String())
			}
			mu.Unlock()
		}
	}()

	c.SetDeadline(time.Now().Add(listenFor + 3*time.Second))
	rd := bufio.NewReader(c)
	lines := make(chan string, 64)
	go func() {
		defer close(lines)
		for {
			l, err := rd.ReadString('\n')
			if err != nil {
				return
			}
			lines <- strings.TrimRight(l, "\r\n")
		}
	}()
	sent := false
	deadline := time.After(listenFor)
loop:
	for {
		select {
		case l, ok := <-lines:
			if !ok {
				break loop
			}
			switch {
			case strings.HasPrefix(l, "V") && t.VersionLine == "":
				t.VersionLine = l
			case strings.HasPrefix(l, "H"):
				t.HandleSeen = true
			case strings.HasPrefix(l, "R") && len(t.Replies) < 4:
				t.Replies = append(t.Replies, truncate(l, 160))
			}
			if t.HandleSeen && !sent {
				sent = true
				fmt.Fprintf(c, "C1|client udpport %d\n", v.LocalPort)
				fmt.Fprintf(c, "C2|sub meter all\n")
			}
		case <-deadline:
			break loop
		case <-ctx.Done():
			break loop
		}
	}
	c.Close()
	udp.Close()
	<-readerDone
	mu.Lock()
	defer mu.Unlock()
	if v.Packets == 0 {
		v.Error = "no UDP from the radio within " + listenFor.String()
	}
	return t
}

func truncate(s string, n int) string {
	if len(s) > n {
		return s[:n] + "…"
	}
	return s
}
