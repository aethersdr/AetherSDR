package main

import (
	"context"
	"log"
	"net"
	"net/netip"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"
)

// Station-device discovery. 4O3A accessories announce themselves by UDP
// broadcast on the station LAN, and the container shares the radio's network
// namespace, so it hears them:
//
//	Antenna Genius   UDP 9007  "AG ip=… port=9007 v=… serial=… name=…"
//	Power Genius XL  UDP 9008  (same key=value form)
//	Tuner Genius XL  UDP 9010  "TunerGenius ip=… v=… serial=… nickname=…"
//
// Discovered devices can be shared over the tailnet automatically as /32
// subnet routes, so a remote AetherSDR reaches them at their LAN addresses.

// discoveryPorts are the 4O3A announcement ports.
var discoveryPorts = []int{9007, 9008, 9010}

// deviceStale is how long a device stays listed after its last announcement.
const deviceStale = 2 * time.Minute

// Device is one discovered station accessory.
type Device struct {
	Kind     string    `json:"kind"` // "Antenna Genius", "Power Genius XL", "Tuner Genius XL"
	Name     string    `json:"name"`
	IP       string    `json:"ip"`
	Port     int       `json:"port"`
	Serial   string    `json:"serial"`
	Version  string    `json:"version"`
	LastSeen time.Time `json:"last_seen"`
}

// parseAnnouncement turns a 4O3A discovery datagram into a Device. The
// announced ip= wins over the packet source; either must be private LAN.
func parseAnnouncement(b []byte, src netip.Addr, port int) (Device, bool) {
	text := strings.TrimSpace(strings.TrimRight(string(b), "\x00"))
	fields := strings.Fields(text)
	if len(fields) < 2 {
		return Device{}, false
	}
	kv := map[string]string{}
	for _, f := range fields[1:] {
		if k, v, ok := strings.Cut(f, "="); ok {
			kv[k] = v
		}
	}
	var d Device
	switch tag := fields[0]; {
	case tag == "AG":
		d.Kind = "Antenna Genius"
	case strings.HasPrefix(tag, "TunerGenius") || tag == "TGXL":
		d.Kind = "Tuner Genius XL"
	case strings.HasPrefix(tag, "PowerGenius") || tag == "PGXL":
		d.Kind = "Power Genius XL"
	default:
		return Device{}, false
	}
	ip := src
	if a, err := netip.ParseAddr(kv["ip"]); err == nil {
		ip = a
	}
	if !ip.Is4() || !ip.IsPrivate() || containerNet.Contains(ip) {
		return Device{}, false
	}
	d.IP = ip.String()
	d.Port = port
	if p := kv["port"]; p != "" {
		var n int
		for _, c := range p {
			if c < '0' || c > '9' {
				n = 0
				break
			}
			n = n*10 + int(c-'0')
		}
		if n > 0 && n < 65536 {
			d.Port = n
		}
	}
	d.Serial = kv["serial"]
	d.Version = kv["v"]
	d.Name = kv["name"]
	if d.Name == "" {
		d.Name = kv["nickname"]
	}
	return d, true
}

// Discovery tracks station devices heard on the LAN.
type Discovery struct {
	OnChange func() // called (not under lock) when the device set changes

	mu      sync.Mutex
	devices map[string]Device // by IP
}

func reuseControl(network, address string, c syscall.RawConn) error {
	var serr error
	err := c.Control(func(fd uintptr) {
		serr = syscall.SetsockoptInt(int(fd), syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
		if serr == nil {
			serr = syscall.SetsockoptInt(int(fd), syscall.SOL_SOCKET, 0xf /* SO_REUSEPORT */, 1)
		}
	})
	if err != nil {
		return err
	}
	return serr
}

// Start listens on every discovery port. A port the radio already owns
// without address reuse is skipped; manual sharing still works.
func (d *Discovery) Start() {
	lc := net.ListenConfig{Control: reuseControl}
	for _, port := range discoveryPorts {
		pc, err := lc.ListenPacket(context.Background(), "udp4", net.JoinHostPort("0.0.0.0", itoa(port)))
		if err != nil {
			log.Printf("discovery: cannot listen on UDP %d: %v", port, err)
			continue
		}
		go d.listen(pc, port)
	}
	go d.expire()
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var b [8]byte
	i := len(b)
	for n > 0 {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
	}
	return string(b[i:])
}

func (d *Discovery) listen(pc net.PacketConn, port int) {
	buf := make([]byte, 2048)
	for {
		n, src, err := pc.ReadFrom(buf)
		if err != nil {
			log.Printf("discovery: UDP %d ended: %v", port, err)
			return
		}
		ap, ok := addrPortOf(src)
		if !ok {
			continue
		}
		dev, ok := parseAnnouncement(buf[:n], ap.Addr(), port)
		if !ok {
			continue
		}
		dev.LastSeen = time.Now()
		d.note(dev)
	}
}

func (d *Discovery) note(dev Device) {
	d.mu.Lock()
	if d.devices == nil {
		d.devices = map[string]Device{}
	}
	prev, existed := d.devices[dev.IP]
	d.devices[dev.IP] = dev
	changed := !existed || prev.Kind != dev.Kind
	d.mu.Unlock()
	if changed {
		log.Printf("discovery: %s %q at %s", dev.Kind, dev.Name, dev.IP)
		if d.OnChange != nil {
			d.OnChange()
		}
	}
}

func (d *Discovery) expire() {
	for {
		time.Sleep(30 * time.Second)
		changed := false
		d.mu.Lock()
		for ip, dev := range d.devices {
			if time.Since(dev.LastSeen) > deviceStale {
				delete(d.devices, ip)
				changed = true
			}
		}
		d.mu.Unlock()
		if changed && d.OnChange != nil {
			d.OnChange()
		}
	}
}

// Devices returns the current devices, sorted by address.
func (d *Discovery) Devices() []Device {
	d.mu.Lock()
	defer d.mu.Unlock()
	out := make([]Device, 0, len(d.devices))
	for _, dev := range d.devices {
		out = append(out, dev)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].IP < out[j].IP })
	return out
}
