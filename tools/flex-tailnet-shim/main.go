// Command flex-tailnet-shim runs inside a FLEX-8000/Aurora radio's waveform
// container and makes the radio a node on a Tailscale tailnet. AetherSDR on
// any tailnet device (including behind CGNAT) connects to the node's tailnet
// IP with its manual/routed connect, and the shim relays the SmartSDR control
// connection and its VITA-49 UDP streams to the radio.
//
// Prototype. See README.md for the design and the measured evidence.
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"net"
	"net/netip"
	"os"
	"strings"
	"time"

	"tailscale.com/ipn/ipnstate"

	// The container base has no CA bundle; compile in Mozilla's roots.
	_ "golang.org/x/crypto/x509roots/fallback"
	"tailscale.com/tsnet"
)

func env(k, def string) string {
	if v := strings.TrimSpace(os.Getenv(k)); v != "" {
		return v
	}
	return def
}

// readSecret returns the auth key from TS_AUTHKEY or, failing that, from a
// file baked into the image at install time.
func readSecret(path string) string {
	if v := strings.TrimSpace(os.Getenv("TS_AUTHKEY")); v != "" {
		return v
	}
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

func main() {
	var (
		hostname  = flag.String("hostname", env("SHIM_HOSTNAME", "flex-radio"), "tailnet node name")
		stateDir  = flag.String("state", env("SHIM_STATE_DIR", "/var/lib/flex-tailnet-shim"), "tsnet state directory")
		keyFile   = flag.String("authkey-file", env("SHIM_AUTHKEY_FILE", "/etc/flex-tailnet-shim/authkey"), "file holding a Tailscale auth key")
		radioAddr = flag.String("radio", env("SSDR_RADIO_ADDRESS", "172.30.1.1"), "radio API address (set by the radio)")
		allow     = flag.String("allow", env("SHIM_ALLOW", ""), "comma-separated tailnet logins or tags allowed to connect (empty = any tailnet peer)")
		maxMTU    = flag.Int("max-mtu", 1200, "clamp client network_mtu= to this many bytes (0 = no clamp)")
	)
	flag.Parse()
	log.SetFlags(log.LstdFlags | log.LUTC)
	startLogSink()

	srv := &tsnet.Server{
		Hostname: *hostname,
		Dir:      *stateDir,
		AuthKey:  readSecret(*keyFile),
		Logf:     func(string, ...any) {}, // tsnet is chatty; our own log covers sessions
		UserLogf: log.Printf,
	}
	defer srv.Close()

	// The radio starts containers about 20 s after boot, before SmartSDR's
	// API is listening and before NTP has corrected the clock. Keep retrying
	// rather than exiting: nothing is known to restart a container that dies.
	var st *ipnstate.Status
	for attempt := 1; ; attempt++ {
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
		s, err := srv.Up(ctx)
		cancel()
		if err == nil {
			st = s
			break
		}
		log.Printf("tailnet up attempt %d failed: %v", attempt, err)
		time.Sleep(min(time.Duration(attempt)*10*time.Second, 2*time.Minute))
	}
	var ip4 netip.Addr
	for _, ip := range st.TailscaleIPs {
		if ip.Is4() {
			ip4 = ip
		}
	}
	if !ip4.IsValid() {
		log.Fatalf("no tailnet IPv4 address")
	}
	log.Printf("tailnet node %q up at %s", *hostname, ip4)

	lc, err := srv.LocalClient()
	if err != nil {
		log.Fatalf("local client: %v", err)
	}
	allowed := map[string]bool{}
	for _, a := range strings.Split(*allow, ",") {
		if a = strings.TrimSpace(a); a != "" {
			allowed[a] = true
		}
	}
	authorize := func(remote net.Addr) (string, bool) {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		who, err := lc.WhoIs(ctx, remote.String())
		if err != nil {
			return "unknown peer: " + err.Error(), false
		}
		id := who.Node.ComputedName
		if who.UserProfile != nil && !who.Node.IsTagged() {
			id = who.UserProfile.LoginName + " on " + id
		}
		if len(allowed) == 0 {
			return id, true
		}
		if who.UserProfile != nil && allowed[who.UserProfile.LoginName] {
			return id, true
		}
		for _, t := range who.Node.Tags {
			if allowed[t] {
				return id, true
			}
		}
		return id, false
	}

	listenUDP := func(port int) net.PacketConn {
		pc, err := srv.ListenPacket("udp4", fmt.Sprintf("%s:%d", ip4, port))
		if err != nil {
			log.Fatalf("tailnet UDP %d: %v", port, err)
		}
		return pc
	}
	ln, err := srv.Listen("tcp", fmt.Sprintf(":%d", radioAPIPort))
	if err != nil {
		log.Fatalf("tailnet TCP %d: %v", radioAPIPort, err)
	}

	relay := &Relay{
		RadioAddr: *radioAddr,
		DialRadio: func() (net.Conn, error) {
			return net.DialTimeout("tcp", net.JoinHostPort(*radioAddr, fmt.Sprint(radioAPIPort)), 5*time.Second)
		},
		HostUDP: func() (*net.UDPConn, error) {
			// Ephemeral port on the radio's network namespace. The kernel's
			// ephemeral range never overlaps SmartSDR's fixed ports.
			return net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero})
		},
		ClientTX:    listenUDP(radioVitaInPort),
		ClientPrime: listenUDP(radioPrimePort),
		ClientOut:   listenUDP(4993),
		Authorize:   authorize,
		MaxMTU:      *maxMTU,
	}
	go reportHealth(lc)
	log.Fatal(relay.Serve(ln))
}

// reportHealth logs the shim's footprint and, for each active peer, whether
// the tunnel is direct or relayed through DERP.
func reportHealth(lc interface {
	Status(context.Context) (*ipnstate.Status, error)
}) {
	for {
		time.Sleep(30 * time.Second)
		rss, cpu := "?", "?"
		if b, err := os.ReadFile("/proc/self/status"); err == nil {
			for _, l := range strings.Split(string(b), "\n") {
				if strings.HasPrefix(l, "VmRSS:") {
					rss = strings.TrimSpace(strings.TrimPrefix(l, "VmRSS:"))
				}
			}
		}
		if b, err := os.ReadFile("/proc/self/stat"); err == nil {
			if f := strings.Fields(string(b)); len(f) > 15 {
				cpu = f[13] + "+" + f[14] + " ticks"
			}
		}
		line := fmt.Sprintf("health rss=%s cpu=%s", rss, cpu)
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		if st, err := lc.Status(ctx); err == nil {
			for _, p := range st.Peer {
				if p.Active {
					path := "direct " + p.CurAddr
					if p.CurAddr == "" {
						path = "DERP " + p.Relay
					}
					line += fmt.Sprintf(" | peer %s %s rx=%d tx=%d", p.HostName, path, p.RxBytes, p.TxBytes)
				}
			}
		}
		cancel()
		log.Print(line)
	}
}
