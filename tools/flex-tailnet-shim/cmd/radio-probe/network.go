package main

import (
	"context"
	"crypto/tls"
	"fmt"
	"net"
	"net/netip"
	"sort"
	"strconv"
	"strings"
	"time"

	_ "golang.org/x/crypto/x509roots/fallback" // as in the shim: no CA bundle on the radio
	"tailscale.com/net/stun"
)

type NetworkInfo struct {
	Interfaces    []Iface      `json:"interfaces"`
	LocalIPv4     []string     `json:"local_ipv4"`
	Routes        string       `json:"routes"`
	ResolvConf    string       `json:"resolv_conf"`
	DNS           []DNSResult  `json:"dns"`
	ControlTCP    Result       `json:"controlplane_tcp_443"`
	ControlTLS    Result       `json:"controlplane_tls"`
	STUN          []STUNResult `json:"stun"`
	ShimPortBinds []BindResult `json:"shim_port_binds"`
	Listening     []string     `json:"listening"`
}

type Iface struct {
	Name  string   `json:"name"`
	MTU   int      `json:"mtu"`
	Flags string   `json:"flags"`
	Addrs []string `json:"addrs"`
}

type Result struct {
	OK    bool   `json:"ok"`
	Ms    int    `json:"ms"`
	Error string `json:"error,omitempty"`
	Info  string `json:"info,omitempty"`
}

type DNSResult struct {
	Host string `json:"host"`
	Result
}

type STUNResult struct {
	Server string `json:"server"`
	// Mapped is the radio's public address as the STUN server saw it.
	Mapped string `json:"mapped,omitempty"`
	Result
}

type BindResult struct {
	Addr string `json:"addr"`
	Result
}

const controlHost = "controlplane.tailscale.com"

// stunServers are Tailscale DERP servers, which answer STUN on 3478 just as
// the shim's netcheck asks them.
var stunServers = []string{"derp1.tailscale.com:3478", "derp10.tailscale.com:3478", "derp4.tailscale.com:3478"}

func errString(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}

func networkInfo(ctx context.Context) NetworkInfo {
	n := NetworkInfo{
		Routes:     readTrim("/proc/net/route", 4096),
		ResolvConf: readTrim("/etc/resolv.conf", 2048),
	}
	ifs, _ := net.Interfaces()
	for _, ifc := range ifs {
		i := Iface{Name: ifc.Name, MTU: ifc.MTU, Flags: ifc.Flags.String()}
		addrs, _ := ifc.Addrs()
		for _, a := range addrs {
			i.Addrs = append(i.Addrs, a.String())
			if ipn, ok := a.(*net.IPNet); ok && ifc.Flags&net.FlagLoopback == 0 {
				if ip4 := ipn.IP.To4(); ip4 != nil {
					n.LocalIPv4 = append(n.LocalIPv4, ip4.String())
				}
			}
		}
		n.Interfaces = append(n.Interfaces, i)
	}

	for _, h := range []string{controlHost, "login.tailscale.com", "derp1.tailscale.com"} {
		cctx, cancel := context.WithTimeout(ctx, 5*time.Second)
		t := time.Now()
		addrs, err := net.DefaultResolver.LookupHost(cctx, h)
		cancel()
		n.DNS = append(n.DNS, DNSResult{h, Result{OK: err == nil, Ms: ms(t), Error: errString(err), Info: strings.Join(addrs, " ")}})
	}

	t := time.Now()
	c, err := net.DialTimeout("tcp", net.JoinHostPort(controlHost, "443"), 8*time.Second)
	n.ControlTCP = Result{OK: err == nil, Ms: ms(t), Error: errString(err)}
	if err == nil {
		n.ControlTCP.Info = c.RemoteAddr().String()
		c.Close()
	}

	// A verified handshake proves the compiled-in roots work and the clock
	// is close enough (a radio's clock is wrong until NTP syncs).
	t = time.Now()
	tc, err := tls.DialWithDialer(&net.Dialer{Timeout: 8 * time.Second}, "tcp", net.JoinHostPort(controlHost, "443"), &tls.Config{ServerName: controlHost})
	n.ControlTLS = Result{OK: err == nil, Ms: ms(t), Error: errString(err)}
	if err == nil {
		st := tc.ConnectionState()
		n.ControlTLS.Info = fmt.Sprintf("%s %s", tls.VersionName(st.Version), tls.CipherSuiteName(st.CipherSuite))
		tc.Close()
	}

	n.STUN = stunChecks(stunServers, 3*time.Second)

	for _, a := range []string{"tcp :48992", "tcp :48993", "udp :41641"} {
		netw, addr, _ := strings.Cut(a, " ")
		r := BindResult{Addr: a}
		var cerr error
		if netw == "tcp" {
			var l net.Listener
			if l, cerr = net.Listen("tcp4", addr); cerr == nil {
				l.Close()
			}
		} else {
			var pc net.PacketConn
			if pc, cerr = net.ListenPacket("udp4", addr); cerr == nil {
				pc.Close()
			}
		}
		r.OK, r.Error = cerr == nil, errString(cerr)
		n.ShimPortBinds = append(n.ShimPortBinds, r)
	}

	for _, f := range []string{"tcp", "tcp6", "udp", "udp6"} {
		n.Listening = append(n.Listening, listening(f, readTrim("/proc/net/"+f, 0))...)
	}
	sort.Strings(n.Listening)
	return n
}

func ms(t time.Time) int { return int(time.Since(t).Milliseconds()) }

func stunChecks(servers []string, wait time.Duration) []STUNResult {
	var out []STUNResult
	for _, s := range servers {
		r := STUNResult{Server: s}
		t := time.Now()
		ua, err := net.ResolveUDPAddr("udp4", s)
		if err != nil {
			r.Error = err.Error()
			out = append(out, r)
			continue
		}
		pc, err := net.ListenUDP("udp4", nil)
		if err != nil {
			r.Error = err.Error()
			out = append(out, r)
			continue
		}
		tx := stun.NewTxID()
		_, err = pc.WriteToUDP(stun.Request(tx), ua)
		buf := make([]byte, 1500)
		for err == nil {
			pc.SetReadDeadline(time.Now().Add(wait))
			var k int
			if k, _, err = pc.ReadFromUDP(buf); err != nil {
				break
			}
			got, mapped, perr := stun.ParseResponse(buf[:k])
			if perr == nil && got == tx {
				r.OK, r.Mapped = true, mapped.String()
				break
			}
		}
		pc.Close()
		r.Ms, r.Error = ms(t), errString(err)
		if r.OK {
			r.Error = ""
		}
		out = append(out, r)
	}
	return out
}

// listening turns a /proc/net/{tcp,udp}[6] table into "proto addr:port"
// entries: TCP sockets in LISTEN, and every bound UDP socket.
func listening(proto, table string) []string {
	var out []string
	seen := map[string]bool{}
	lines := strings.Split(table, "\n")
	for _, l := range lines[min(1, len(lines)):] {
		f := strings.Fields(l)
		if len(f) < 4 {
			continue
		}
		if strings.HasPrefix(proto, "tcp") && f[3] != "0A" {
			continue
		}
		ap, ok := parseProcAddr(f[1])
		if !ok {
			continue
		}
		e := proto + " " + ap.String()
		if !seen[e] {
			seen[e] = true
			out = append(out, e)
		}
	}
	return out
}

// parseProcAddr decodes /proc/net's "0100007F:1388" (IPv4) or 32-hex-digit
// IPv6 form. The kernel prints each 32-bit word in host (little-endian on
// ARM) order.
func parseProcAddr(s string) (netip.AddrPort, bool) {
	h, p, ok := strings.Cut(s, ":")
	if !ok {
		return netip.AddrPort{}, false
	}
	port, err := strconv.ParseUint(p, 16, 16)
	if err != nil || (len(h) != 8 && len(h) != 32) {
		return netip.AddrPort{}, false
	}
	var b [16]byte
	for w := 0; w < len(h)/8; w++ {
		v, err := strconv.ParseUint(h[w*8:w*8+8], 16, 32)
		if err != nil {
			return netip.AddrPort{}, false
		}
		b[w*4], b[w*4+1], b[w*4+2], b[w*4+3] = byte(v), byte(v>>8), byte(v>>16), byte(v>>24)
	}
	var a netip.Addr
	if len(h) == 8 {
		a = netip.AddrFrom4([4]byte(b[:4]))
	} else {
		a = netip.AddrFrom16(b).Unmap()
	}
	return netip.AddrPortFrom(a, uint16(port)), true
}
