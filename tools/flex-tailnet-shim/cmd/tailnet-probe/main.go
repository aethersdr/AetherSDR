// Command tailnet-probe proves a remote path through the in-radio tailnet
// shim from anywhere, including a machine on the radio's own LAN: it joins
// the tailnet as an ephemeral node with its own userspace network stack, so
// its connections can only travel over the tailnet, never the local LAN.
//
// It accepts the tailnet's subnet routes, then dials each target (the radio
// API at the shim's tailnet address, station devices at their LAN addresses)
// and reports whether a TCP connection was made and what the far end sent.
//
//	tailnet-probe -authkey-file key.txt 100.x.y.z:4992 192.168.50.103:9007
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"net/netip"
	"os"
	"strings"
	"time"

	"tailscale.com/client/local"
	"tailscale.com/ipn"
	"tailscale.com/tsnet"
)

func main() {
	keyFile := flag.String("authkey-file", "", "file holding a Tailscale auth key (an ephemeral key is best)")
	hostname := flag.String("hostname", "tailnet-probe", "tailnet machine name for this probe")
	wait := flag.Duration("route-wait", 60*time.Second, "how long to wait for subnet routes to appear")
	flag.Parse()
	if *keyFile == "" || flag.NArg() == 0 {
		fmt.Fprintln(os.Stderr, "usage: tailnet-probe -authkey-file FILE host:port...")
		os.Exit(2)
	}
	key, err := os.ReadFile(*keyFile)
	if err != nil {
		log.Fatalf("auth key: %v", err)
	}
	dir, err := os.MkdirTemp("", "tailnet-probe-")
	if err != nil {
		log.Fatal(err)
	}
	defer os.RemoveAll(dir)

	srv := &tsnet.Server{
		Hostname:  *hostname,
		Dir:       dir,
		AuthKey:   strings.TrimSpace(string(key)),
		Ephemeral: true,
		Logf:      func(string, ...any) {},
	}
	defer srv.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	if _, err := srv.Up(ctx); err != nil {
		log.Fatalf("join tailnet: %v", err)
	}
	lc, err := srv.LocalClient()
	if err != nil {
		log.Fatal(err)
	}
	// Accept subnet routes, like a Windows or macOS client does by default.
	if _, err := lc.EditPrefs(ctx, &ipn.MaskedPrefs{Prefs: ipn.Prefs{RouteAll: true}, RouteAllSet: true}); err != nil {
		log.Fatalf("accept routes: %v", err)
	}
	fmt.Printf("joined the tailnet as %q\n", *hostname)

	// Wait until every LAN target is covered by some peer's primary route.
	deadline := time.Now().Add(*wait)
	for _, t := range flag.Args() {
		ap, err := netip.ParseAddrPort(t)
		if err != nil {
			log.Fatalf("%s: %v", t, err)
		}
		if ap.Addr().IsPrivate() {
			for {
				if via := routeFor(ctx, lc, ap.Addr()); via != "" {
					fmt.Printf("route   %-22s via %s\n", ap.Addr(), via)
					break
				}
				if time.Now().After(deadline) {
					fmt.Printf("route   %-22s NONE: not approved, or not advertised\n", ap.Addr())
					break
				}
				time.Sleep(2 * time.Second)
			}
		}
	}

	failed := false
	for _, t := range flag.Args() {
		dctx, dcancel := context.WithTimeout(ctx, 10*time.Second)
		start := time.Now()
		c, err := srv.Dial(dctx, "tcp", t)
		dcancel()
		if err != nil {
			fmt.Printf("dial    %-22s FAILED after %v: %v\n", t, time.Since(start).Round(time.Millisecond), err)
			failed = true
			continue
		}
		rtt := time.Since(start).Round(time.Millisecond)
		c.SetReadDeadline(time.Now().Add(3 * time.Second))
		buf := make([]byte, 160)
		n, _ := c.Read(buf)
		c.Close()
		greeting := strings.TrimSpace(strings.Map(func(r rune) rune {
			if r < 32 || r > 126 {
				return ' '
			}
			return r
		}, string(buf[:n])))
		fmt.Printf("dial    %-22s OK in %v; first %d bytes: %q\n", t, rtt, n, greeting)
	}
	if failed {
		os.Exit(1)
	}
}

// routeFor returns the tailnet peer whose approved primary routes cover
// addr, or "".
func routeFor(ctx context.Context, lc *local.Client, addr netip.Addr) string {
	st, err := lc.Status(ctx)
	if err != nil {
		return ""
	}
	for _, p := range st.Peer {
		if p.PrimaryRoutes == nil {
			continue
		}
		for _, r := range p.PrimaryRoutes.All() {
			if r.Contains(addr) {
				return fmt.Sprintf("%s (%s)", p.HostName, r)
			}
		}
	}
	return ""
}
