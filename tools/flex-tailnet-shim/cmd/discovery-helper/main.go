// Command discovery-helper lists a tailnet radio in SmartSDR for Windows.
//
// SmartSDR for Windows finds radios only by their discovery broadcast, and
// has no connect-by-address. A tailnet carries no broadcast, and the copy
// the shim sends each device reaches only one of the programs listening on
// UDP 4992 there, while SmartSDR, DAX and CAT each need it. So this helper
// runs on the PC: it connects to the radio's tailnet shim, receives each
// announcement (already naming the shim's tailnet address), and broadcasts
// it on the PC, where every listener gets it.
//
//	discovery-helper -shim flex-radio
//
// It keeps no copy: when the shim can't be reached, nothing is broadcast,
// and the radio drops out of the list as it would on a LAN.
package main

import (
	"bytes"
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"time"
)

// feedPort is the shim's discovery helper port (AnnounceFeedPort).
const feedPort = 48994

// maxFrame bounds an announcement; a radio's is under 1 KB.
const maxFrame = 2048

func main() {
	shim := flag.String("shim", "", "the radio's tailnet name or address, e.g. flex-radio")
	bcast := flag.String("broadcast", "255.255.255.255", "address to broadcast the announcement to")
	from := flag.String("from", "", "local address to broadcast from (default: the system's choice)")
	flag.Parse()
	if *shim == "" {
		flag.Usage()
		log.Fatal("-shim is required")
	}
	dst, err := net.ResolveUDPAddr("udp4", net.JoinHostPort(*bcast, "4992"))
	if err != nil {
		log.Fatalf("-broadcast: %v", err)
	}
	var laddr *net.UDPAddr
	if *from != "" {
		if laddr, err = net.ResolveUDPAddr("udp4", net.JoinHostPort(*from, "0")); err != nil {
			log.Fatalf("-from: %v", err)
		}
	}
	// Go sets SO_BROADCAST on every UDP socket.
	out, err := net.ListenUDP("udp4", laddr)
	if err != nil {
		log.Fatalf("broadcast socket: %v", err)
	}
	send := func(b []byte) error {
		_, err := out.WriteToUDP(b, dst)
		return err
	}
	addr := net.JoinHostPort(*shim, fmt.Sprint(feedPort))
	for {
		c, err := net.DialTimeout("tcp", addr, 10*time.Second)
		if err != nil {
			log.Printf("cannot reach %s: %v; retrying", addr, err)
			time.Sleep(5 * time.Second)
			continue
		}
		log.Printf("connected to %s; broadcasting its announcements to %s", addr, dst)
		err = relay(c, send)
		c.Close()
		log.Printf("disconnected from %s: %v; retrying", addr, err)
		time.Sleep(5 * time.Second)
	}
}

// relay reads announcements from the shim (a 2-byte big-endian length, then
// the packet) and broadcasts each, until the connection fails. A frame that
// isn't an announcement is skipped; a malformed length ends the connection.
func relay(r io.Reader, send func([]byte) error) error {
	var n [2]byte
	buf := make([]byte, maxFrame)
	started, failing := false, false
	for {
		if _, err := io.ReadFull(r, n[:]); err != nil {
			return err
		}
		size := int(binary.BigEndian.Uint16(n[:]))
		if size == 0 || size > maxFrame {
			return fmt.Errorf("bad frame length %d", size)
		}
		b := buf[:size]
		if _, err := io.ReadFull(r, b); err != nil {
			return err
		}
		if !bytes.Contains(b, []byte("serial=")) {
			continue
		}
		if err := send(b); err != nil {
			if !failing {
				log.Printf("broadcasting: %v", err)
				failing = true
			}
			continue
		}
		failing = false
		if !started {
			log.Printf("broadcasting the radio's announcement")
			started = true
		}
	}
}
