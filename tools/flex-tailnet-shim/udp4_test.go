package main

import (
	"errors"
	"net"
	"net/netip"
	"testing"
	"time"

	"tailscale.com/net/packet"
)

// An injected packet must be one Tailscale parses as the same UDP datagram,
// with valid IPv4 and UDP checksums: a bad checksum is dropped by the client's
// stack, which would look like radio-side loss.
func TestBuildUDP4RoundTrips(t *testing.T) {
	src := netip.MustParseAddrPort("100.66.239.114:4993")
	dst := netip.MustParseAddrPort("100.83.193.10:61139")
	for _, n := range []int{0, 1, 7, 1136, injectMaxPacket - 28} {
		pl := make([]byte, n)
		for i := range pl {
			pl[i] = byte(i*7 + 3)
		}
		pkt := buildUDP4(src, dst, pl)
		var q packet.Parsed
		q.Decode(pkt)
		if q.IPProto != 17 || q.Src != src || q.Dst != dst || string(q.Payload()) != string(pl) {
			t.Fatalf("n=%d: decoded proto %v %v->%v, %d-byte payload", n, q.IPProto, q.Src, q.Dst, len(q.Payload()))
		}
		if foldSum(sum16(pkt[:20], 0)) != 0xffff {
			t.Fatalf("n=%d: bad IPv4 header checksum", n)
		}
		s4, d4 := src.Addr().As4(), dst.Addr().As4()
		acc := sum16(s4[:], 0)
		acc = sum16(d4[:], acc)
		acc += 17 + uint32(8+n)
		if foldSum(sum16(pkt[20:], acc)) != 0xffff {
			t.Fatalf("n=%d: bad UDP checksum", n)
		}
	}
}

func TestBuildUDP4NeverReusesTheBuffer(t *testing.T) {
	src := netip.MustParseAddrPort("100.64.0.1:4993")
	dst := netip.MustParseAddrPort("100.64.0.2:5000")
	a := buildUDP4(src, dst, []byte("one"))
	b := buildUDP4(src, dst, []byte("two"))
	if &a[0] == &b[0] {
		t.Fatal("buildUDP4 reused its buffer; InjectOutbound owns each packet")
	}
}

// toClient injects what fits the tailnet MTU and sends the rest, and anything
// Inject refuses, through ClientOut, so no datagram is dropped by the choice.
func TestToClientInjectsWhatFitsAndFallsBack(t *testing.T) {
	out := loopUDP(t)
	sink := loopUDP(t)
	dst := sink.LocalAddr().(*net.UDPAddr).AddrPort()
	var injected [][]byte
	refuse := false
	r := &Relay{
		ClientOut: out,
		InjectSrc: netip.MustParseAddrPort("127.0.0.1:4993"),
		Inject: func(p []byte) error {
			if refuse {
				return errors.New("refused")
			}
			injected = append(injected, p)
			return nil
		},
	}
	recv := func() int {
		sink.SetReadDeadline(time.Now().Add(2 * time.Second))
		b := make([]byte, 4096)
		n, err := sink.Read(b)
		if err != nil {
			t.Fatalf("expected a datagram on ClientOut: %v", err)
		}
		return n
	}

	if err := r.toClient(make([]byte, 1136), dst); err != nil || len(injected) != 1 {
		t.Fatalf("a 1136-byte datagram should be injected: err %v, injected %d", err, len(injected))
	}
	if err := r.toClient(make([]byte, injectMaxPacket), dst); err != nil || len(injected) != 1 || recv() != injectMaxPacket {
		t.Fatalf("an oversize datagram should go through ClientOut")
	}
	refuse = true
	if err := r.toClient(make([]byte, 100), dst); err != nil || recv() != 100 {
		t.Fatalf("a datagram Inject refuses should go through ClientOut")
	}
	r.Inject = nil
	if err := r.toClient(make([]byte, 50), dst); err != nil || recv() != 50 {
		t.Fatalf("without Inject everything should go through ClientOut")
	}
}
