package main

import (
	"fmt"
	"log"
	"net"
	"net/netip"
	"syscall"
	"time"
)

// Hermes-Lite 2 discovery. Unlike the 4O3A accessories, an HL2 never
// announces itself: it answers an HPSDR Protocol 1 discovery request
// (EF FE 02, UDP 1024) and sends the reply only to the requester
// (Hermes-Lite2 gateware/rtl ethernet/udp_recv.v:55, network.v:720). So the
// shim asks, the same sweep AetherSDR runs (Hl2Discovery::sweepNow), and the
// HL2 is listed at the address its reply came from.

const (
	hl2Port          = 1024
	hl2SweepInterval = 5 * time.Second // AetherSDR's Hl2Discovery cadence
	hl2BoardID       = 0x06            // Hermes-Lite / Hermes-Lite 2
	hl2Kind          = "Hermes-Lite 2"
)

// hl2DiscoveryRequest is the 63-byte Protocol 1 discovery request.
func hl2DiscoveryRequest() []byte {
	b := make([]byte, 63)
	b[0], b[1], b[2] = 0xEF, 0xFE, 0x02
	return b
}

// parseHL2Reply turns a discovery reply into a Device at the reply's source
// address, which must be private LAN outside the container network. Status
// 0x02 is idle, 0x03 streaming to another client; both are the HL2 present.
// The MAC is its identity, rendered as AetherSDR's Hl2Discovery::macToSerial
// does, so both name the same radio.
func parseHL2Reply(b []byte, src netip.AddrPort) (Device, bool) {
	if len(b) < 11 || b[0] != 0xEF || b[1] != 0xFE || (b[2] != 0x02 && b[2] != 0x03) {
		return Device{}, false
	}
	if b[10] != hl2BoardID || src.Port() != hl2Port {
		return Device{}, false
	}
	ip := src.Addr().Unmap()
	if !ip.Is4() || !ip.IsPrivate() || containerNet.Contains(ip) {
		return Device{}, false
	}
	return Device{
		Kind:    hl2Kind,
		Name:    hl2Kind,
		IP:      ip.String(),
		Port:    hl2Port,
		Serial:  fmt.Sprintf("%02X:%02X:%02X:%02X:%02X:%02X", b[3], b[4], b[5], b[6], b[7], b[8]),
		Version: itoa(int(b[9])),
	}, true
}

// lanBroadcasts returns the directed broadcast address of every LAN the
// host is on, leaving out the container network (the radio's internal link
// to its FPGA and containers).
func lanBroadcasts() []netip.Addr {
	ifaces, err := net.Interfaces()
	if err != nil {
		return nil
	}
	var out []netip.Addr
	for _, ifc := range ifaces {
		if ifc.Flags&net.FlagUp == 0 || ifc.Flags&net.FlagBroadcast == 0 || ifc.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, err := ifc.Addrs()
		if err != nil {
			continue
		}
		for _, a := range addrs {
			ipn, ok := a.(*net.IPNet)
			if !ok {
				continue
			}
			p, ok := netip.AddrFromSlice(ipn.IP.To4())
			if !ok {
				continue
			}
			ones, bits := ipn.Mask.Size()
			if b, ok := directedBroadcast(netip.PrefixFrom(p, ones), bits); ok {
				out = append(out, b)
			}
		}
	}
	return out
}

// directedBroadcast is the broadcast address of a private IPv4 LAN prefix.
func directedBroadcast(p netip.Prefix, bits int) (netip.Addr, bool) {
	a := p.Addr()
	if bits != 32 || !a.Is4() || !a.IsPrivate() || containerNet.Contains(a) || p.Bits() < 8 || p.Bits() > 30 {
		return netip.Addr{}, false
	}
	b := a.As4()
	host := uint32(1)<<(32-p.Bits()) - 1
	v := (uint32(b[0])<<24 | uint32(b[1])<<16 | uint32(b[2])<<8 | uint32(b[3])) | host
	return netip.AddrFrom4([4]byte{byte(v >> 24), byte(v >> 16), byte(v >> 8), byte(v)}), true
}

// sweepHL2 probes for HL2s every hl2SweepInterval while SweepHL2 allows,
// and lists each reply.
func (d *Discovery) sweepHL2() {
	pc, err := net.ListenUDP("udp4", &net.UDPAddr{})
	if err != nil {
		log.Printf("discovery: HL2 sweep disabled: %v", err)
		return
	}
	if raw, err := pc.SyscallConn(); err == nil {
		_ = raw.Control(func(fd uintptr) {
			_ = syscall.SetsockoptInt(int(fd), syscall.SOL_SOCKET, syscall.SO_BROADCAST, 1)
		})
	}
	go func() {
		buf := make([]byte, 2048)
		for {
			n, src, err := pc.ReadFromUDPAddrPort(buf)
			if err != nil {
				log.Printf("discovery: HL2 sweep ended: %v", err)
				return
			}
			if dev, ok := parseHL2Reply(buf[:n], src); ok {
				dev.LastSeen = time.Now()
				d.note(dev)
			}
		}
	}()
	req := hl2DiscoveryRequest()
	for {
		if d.SweepHL2 == nil || !d.SweepHL2() {
			time.Sleep(hl2SweepInterval)
			continue
		}
		for _, b := range lanBroadcasts() {
			_, _ = pc.WriteToUDPAddrPort(req, netip.AddrPortFrom(b, hl2Port))
		}
		time.Sleep(hl2SweepInterval)
	}
}
