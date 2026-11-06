package main

import (
	"encoding/binary"
	"net/netip"
	"sync/atomic"
)

// Radio -> client VITA-49 skips the userspace netstack: the relay builds the
// IPv4+UDP packet itself and hands it to the tailnet device (tstun's
// InjectOutbound), which records UDP flow state, encrypts and sends it as it
// would a netstack packet. On a FLEX-6000's single core the netstack's UDP
// write (a route lookup per packet, a buffer copy and a goroutine hand-off)
// cost about 0.3 ms of every 0.8–0.9 ms the shim spent per packet (#6990).

// injectMaxPacket is the tailnet MTU. Larger datagrams take the netstack
// path, which fragments; the shim clamps the radio's network MTU below this
// (mtuInjection), so in practice every VITA-49 datagram is injected.
const injectMaxPacket = 1280

var ipv4ID atomic.Uint32

// buildUDP4 returns a new IPv4+UDP packet carrying payload from src to dst.
// InjectOutbound takes ownership of the slice, so it is never reused.
func buildUDP4(src, dst netip.AddrPort, payload []byte) []byte {
	total := 20 + 8 + len(payload)
	p := make([]byte, total)
	p[0] = 0x45 // IPv4, 20-byte header
	binary.BigEndian.PutUint16(p[2:], uint16(total))
	binary.BigEndian.PutUint16(p[4:], uint16(ipv4ID.Add(1)))
	p[6] = 0x40 // don't fragment
	p[8] = 64   // TTL
	p[9] = 17   // UDP
	s4, d4 := src.Addr().As4(), dst.Addr().As4()
	copy(p[12:16], s4[:])
	copy(p[16:20], d4[:])
	binary.BigEndian.PutUint16(p[10:], ^foldSum(sum16(p[:20], 0)))

	u := p[20:]
	binary.BigEndian.PutUint16(u[0:], src.Port())
	binary.BigEndian.PutUint16(u[2:], dst.Port())
	binary.BigEndian.PutUint16(u[4:], uint16(8+len(payload)))
	copy(u[8:], payload)
	acc := sum16(s4[:], 0)
	acc = sum16(d4[:], acc)
	acc += 17 + uint32(8+len(payload))
	c := ^foldSum(sum16(u, acc))
	if c == 0 {
		c = 0xffff // RFC 768: a computed zero is sent as all ones
	}
	binary.BigEndian.PutUint16(u[6:], c)
	return p
}

// sum16 adds b to acc as big-endian 16-bit words (RFC 1071).
func sum16(b []byte, acc uint32) uint32 {
	n := len(b) &^ 1
	for i := 0; i < n; i += 2 {
		acc += uint32(b[i])<<8 | uint32(b[i+1])
	}
	if len(b)&1 == 1 {
		acc += uint32(b[len(b)-1]) << 8
	}
	return acc
}

func foldSum(acc uint32) uint16 {
	for acc>>16 != 0 {
		acc = acc&0xffff + acc>>16
	}
	return uint16(acc)
}
