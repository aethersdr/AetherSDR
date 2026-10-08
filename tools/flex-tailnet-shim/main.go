// Command flex-tailnet-shim runs inside a FLEX-8000/Aurora radio's waveform
// container and makes the radio a node on a Tailscale tailnet. AetherSDR on
// any tailnet device (including behind CGNAT) connects to the node's tailnet
// IP with its manual/routed connect, and the shim relays the SmartSDR control
// connection and its VITA-49 UDP streams to the radio.
//
// The image is generic and carries no secret. AetherSDR provisions the
// Tailscale auth key afterwards over the LAN (see api.go).
//
// Prototype. See README.md for the design and the measured evidence.
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"strings"
	"time"

	"tailscale.com/ipn/ipnstate"

	// The container has no CA bundle; compile in Mozilla's roots.
	_ "golang.org/x/crypto/x509roots/fallback"
)

const shimVersion = "0.3.2"

func env(k, def string) string {
	if v := strings.TrimSpace(os.Getenv(k)); v != "" {
		return v
	}
	return def
}

func main() {
	var (
		stateDir  = flag.String("state", env("SHIM_STATE_DIR", "/var/lib/flex-tailnet-shim"), "state directory (persists across reboots)")
		radioAddr = flag.String("radio", env("SSDR_RADIO_ADDRESS", "172.30.1.1"), "radio API address (set by the radio)")
		apiAddr   = flag.String("api", env("SHIM_API_ADDR", fmt.Sprintf(":%d", ProvisionPort)), "provisioning API listen address")
		maxMTU    = flag.Int("max-mtu", 1200, "clamp client network_mtu= to this many bytes (0 = no clamp)")
	)
	flag.Parse()
	log.SetFlags(log.LstdFlags | log.LUTC)
	startLogSink()
	log.Printf("flex-tailnet-shim %s starting", shimVersion)

	disc := &Discovery{}
	node := &Node{StateDir: *stateDir, RadioAddr: *radioAddr, MaxMTU: *maxMTU, Discovery: disc}
	disc.OnChange = node.RefreshRoutes
	disc.Start()
	if err := node.load(); err != nil {
		log.Printf("state: %v; starting unprovisioned", err)
	}
	node.Boot()
	ServeAPI(node, *apiAddr)
}

// reportHealth logs the shim's footprint and, for each active peer, whether
// the tunnel is direct or relayed through DERP. It ends when the tailnet
// node it was started for goes away.
func reportHealth(lc interface {
	Status(context.Context) (*ipnstate.Status, error)
}) {
	for {
		time.Sleep(30 * time.Second)
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		st, err := lc.Status(ctx)
		cancel()
		if err != nil {
			return
		}
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
		for _, p := range st.Peer {
			if p.Active {
				path := "direct " + p.CurAddr
				if p.CurAddr == "" {
					path = "DERP " + p.Relay
				}
				line += fmt.Sprintf(" | peer %s %s rx=%d tx=%d", p.HostName, path, p.RxBytes, p.TxBytes)
			}
		}
		log.Print(line)
	}
}
