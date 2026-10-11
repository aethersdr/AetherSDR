package tun

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// GSOType represents the type of segmentation offload.
type GSOType int

const (
	GSONone GSOType = iota
	GSOTCPv4
	GSOTCPv6
	GSOUDPL4
)

func (g GSOType) String() string {
	switch g {
	case GSONone:
		return "GSONone"
	case GSOTCPv4:
		return "GSOTCPv4"
	case GSOTCPv6:
		return "GSOTCPv6"
	case GSOUDPL4:
		return "GSOUDPL4"
	default:
		return "unknown"
	}
}

// GSOOptions is loosely modeled after struct virtio_net_hdr from the VIRTIO
// specification. It is a common representation of GSO metadata that can be
// applied to support packet GSO across tun.Device implementations.
type GSOOptions struct {
	// GSOType represents the type of segmentation offload.
	GSOType GSOType
	// HdrLen is the sum of the layer 3 and 4 header lengths. This field may be
	// zero when GSOType == GSONone.
	HdrLen uint16
	// CsumStart is the head byte index of the packet data to be checksummed,
	// i.e. the start of the TCP or UDP header.
	CsumStart uint16
	// CsumOffset is the offset from CsumStart where the 2-byte checksum value
	// should be placed.
	CsumOffset uint16
	// GSOSize is the size of each segment exclusive of HdrLen. The tail segment
	// may be smaller than this value.
	GSOSize uint16
	// NeedsCsum may be set where GSOType == GSONone. When set, the checksum
	// at CsumStart + CsumOffset must be a partial checksum, i.e. the
	// pseudo-header sum.
	NeedsCsum bool
}

const (
	ipv4SrcAddrOffset = 12
	ipv6SrcAddrOffset = 8
)

const tcpFlagsOffset = 13

const (
	tcpFlagFIN uint8 = 0x01
	tcpFlagPSH uint8 = 0x08
	tcpFlagACK uint8 = 0x10
)

const (
	// defined here in order to avoid importation of any platform-specific pkgs
	ipProtoTCP = 6
	ipProtoUDP = 17
)

// GSOSplit splits 'in' according to options and writes the resulting segments
// into slab. It populates packets with each packet's offset and size and returns
// the number of populated entries.
//
// spacing bytes are reserved before the first packet, between adjacent packets,
// and after the final packet.
//
// 'in' may overlap slab only when in begins at slab[spacing]. Overlapping input
// is modified and must not be used after this call.
//
// If packets or slab cannot accommodate every segment, GSOSplit returns the
// number successfully written together with [ErrTooManySegments]. packets[:n]
// remains valid.
func GSOSplit(in []byte, options GSOOptions, slab []byte, packets []ReadPacket, spacing int) (int, error) {
	if spacing < 0 {
		return 0, errors.New("spacing must be nonnegative")
	}

	// return early if instructed to split with no split size
	if options.GSOType != GSONone && options.GSOSize == 0 {
		return 0, fmt.Errorf("invalid GSOType (%v) with zero GSOSize", options.GSOType)
	}

	// return early if we can't fill a single packet
	if len(packets) == 0 || spacing > len(slab)/2 {
		return 0, ErrTooManySegments
	}

	cSumAt := int(options.CsumStart) + int(options.CsumOffset)
	if cSumAt+1 >= len(in) {
		return 0, fmt.Errorf("end of checksum offset (%d) exceeds packet length (%d)", cSumAt+1, len(in))
	}

	if len(in) < int(options.HdrLen) {
		return 0, fmt.Errorf("length of packet (%d) < GSO HdrLen (%d)", len(in), options.HdrLen)
	}

	// Handle the conditions where we are copying a single element to slab.
	payloadLen := len(in) - int(options.HdrLen)
	if options.GSOType == GSONone || payloadLen < int(options.GSOSize) {
		// trim spacing on both ends
		output := slab[spacing : len(slab)-spacing]

		if len(in) > len(output) {
			return 0, ErrTooManySegments
		}

		// slice to packet size, so checksum computation below is correct
		singleOut := output[:len(in)]

		// Copy before checksum computation, so that we don't mutate 'in' if
		// 'in' and 'slab' happen to be disjoint.
		copy(singleOut, in)
		packets[0] = ReadPacket{
			Offset: spacing,
			Size:   len(singleOut),
		}

		if options.NeedsCsum {
			// The initial value at the checksum offset should be summed with
			// the checksum we compute. This is typically the pseudo-header sum.
			initial := binary.BigEndian.Uint16(singleOut[cSumAt:])
			singleOut[cSumAt], singleOut[cSumAt+1] = 0, 0
			binary.BigEndian.PutUint16(singleOut[cSumAt:], ^Checksum(singleOut[options.CsumStart:], initial))
		}
		return 1, nil
	}

	if options.HdrLen < options.CsumStart {
		return 0, fmt.Errorf("GSO HdrLen (%d) < GSO CsumStart (%d)", options.HdrLen, options.CsumStart)
	}

	ipVersion := in[0] >> 4
	switch ipVersion {
	case 4:
		if options.GSOType != GSOTCPv4 && options.GSOType != GSOUDPL4 {
			return 0, fmt.Errorf("ip header version: %d, GSO type: %s", ipVersion, options.GSOType)
		}
		if len(in) < 20 {
			return 0, fmt.Errorf("length of packet (%d) < minimum ipv4 header size (%d)", len(in), 20)
		}
	case 6:
		if options.GSOType != GSOTCPv6 && options.GSOType != GSOUDPL4 {
			return 0, fmt.Errorf("ip header version: %d, GSO type: %s", ipVersion, options.GSOType)
		}
		if len(in) < 40 {
			return 0, fmt.Errorf("length of packet (%d) < minimum ipv6 header size (%d)", len(in), 40)
		}
	default:
		return 0, fmt.Errorf("invalid ip header version: %d", ipVersion)
	}

	// Calculate how many segments fit in slab. I found it helpful to mentally
	// model the slab as:
	//
	//	[leading spacing][packet][spacing][packet]...[spacing]
	//
	// If every packet were full-sized, after removing the leading spacing each
	// packet consumes fullPacketSize+spacing bytes.
	hdrLen := int(options.HdrLen)
	gsoSize := int(options.GSOSize)
	// equivalent to ceil(payloadLen / gsoSize), but overflow safe if payloadLen
	// is close to bounds
	totalOutSegments := payloadLen / gsoSize
	if payloadLen%gsoSize != 0 {
		totalOutSegments++
	}
	// how many full gso-sized segments would fit in slab
	fullPacketSize := hdrLen + gsoSize
	fullFit := (len(slab) - spacing) / (fullPacketSize + spacing)
	// this min does not yet account for a smaller tail
	n := min(totalOutSegments, len(packets), fullFit)

	// Account for the potentially shorter final segment. The exact layout needs
	// one header per segment, the original payload bytes, and N+1 spacing
	// regions.
	requiredBytesLen :=
		totalOutSegments*hdrLen +
			payloadLen +
			(totalOutSegments+1)*spacing
	if len(packets) >= totalOutSegments && requiredBytesLen <= len(slab) {
		// n was previously calculated assuming totalOutSegments were all of
		// gsoSize, but the tail segment may be smaller than gsoSize. If a
		// smaller than gsoSize tail fits, we can bump n back to totalOutSegments,
		// assuming packets is long enough.
		n = totalOutSegments
	}

	// n is the number of leading segments that fit.
	//
	// Since input and output may overlap, split back-to-front. Splitting
	// front-to-back could overwrite input before it is copied.
	//
	// H is the header; A, B, and C are payload segments.
	//
	//	Input:
	//	[ H | A | B | C ]
	//
	//	Desired output:
	//	[ H | A | gap | H | B | gap | H | C ]
	//
	//	Front-to-back could overwrite unread B/C:
	//	[ H | A | gap | H ... ]
	//	          └── overwrites remaining input ──┘
	//
	// For the same reason, copy each segment's payload before its headers.

	// Identify IP addr offsets and length, transport protocol, and transport offsets.
	iphLen := int(options.CsumStart)
	srcAddrOffset := ipv6SrcAddrOffset
	addrLen := 16
	if ipVersion == 4 {
		srcAddrOffset = ipv4SrcAddrOffset
		addrLen = 4
	}
	transportCsumAt := int(options.CsumStart + options.CsumOffset)
	var firstTCPSeqNum uint32
	var protocol uint8
	if options.GSOType == GSOTCPv4 || options.GSOType == GSOTCPv6 {
		protocol = ipProtoTCP
		if len(in) < int(options.CsumStart)+20 {
			return 0, fmt.Errorf("length of packet (%d) < GSO CsumStart (%d) + minimum TCP header size (%d)",
				len(in), options.CsumStart, 20)
		}
		firstTCPSeqNum = binary.BigEndian.Uint32(in[options.CsumStart+4:])
	} else {
		protocol = ipProtoUDP
	}

	// The segment-writing loop indexes these fixed IP and transport header
	// fields within out[:hdrLen]. Validate that each field fits before
	// modifying slab.
	csumStart := int(options.CsumStart)
	csumAt := csumStart + int(options.CsumOffset)
	if ipVersion == 4 {
		if hdrLen < 20 {
			return 0, fmt.Errorf("IPv4 header exceeds GSO header length %d", hdrLen)
		}
	} else {
		if hdrLen < 40 {
			return 0, fmt.Errorf("IPv6 header exceeds GSO header length %d", hdrLen)
		}
	}
	if protocol == ipProtoTCP {
		if hdrLen < csumStart+20 {
			return 0, fmt.Errorf(
				"TCP header at offset %d exceeds GSO header length %d",
				csumStart, hdrLen,
			)
		}
	} else {
		if hdrLen < csumStart+8 {
			return 0, fmt.Errorf(
				"UDP header at offset %d exceeds GSO header length %d",
				csumStart, hdrLen,
			)
		}
	}
	if hdrLen < csumAt+2 {
		return 0, fmt.Errorf(
			"checksum field at offset %d exceeds GSO header length %d",
			csumAt, hdrLen,
		)
	}

	// Split back-to-front
	for i := n - 1; i >= 0; i-- {
		// identify input and output offsets
		payloadStart := hdrLen + (i * gsoSize)
		payloadEnd := min(payloadStart+gsoSize, len(in))
		payloadSize := payloadEnd - payloadStart
		packetSize := hdrLen + payloadSize
		outOffset := spacing + i*(fullPacketSize+spacing)
		out := slab[outOffset : outOffset+packetSize]
		packets[i] = ReadPacket{
			Offset: outOffset,
			Size:   packetSize,
		}

		// Copy payload first because in and out may overlap.
		copy(out[hdrLen:], in[payloadStart:payloadEnd])
		copy(out[:hdrLen], in[:hdrLen])

		if ipVersion == 4 {
			// For IPv4 we are responsible for incrementing the ID field,
			// updating the total len field, and recalculating the header
			// checksum.
			if i > 0 {
				id := binary.BigEndian.Uint16(out[4:])
				id += uint16(i)
				binary.BigEndian.PutUint16(out[4:], id)
			}
			// clear ipv4 header checksum
			out[10], out[11] = 0, 0
			// update total len
			binary.BigEndian.PutUint16(out[2:], uint16(packetSize))
			// compute & set updated checksum
			binary.BigEndian.PutUint16(out[10:], ^Checksum(out[:iphLen], 0))
		} else {
			// For IPv6 we are responsible for updating the payload length field.
			binary.BigEndian.PutUint16(out[4:], uint16(packetSize-iphLen))
		}

		if protocol == ipProtoTCP {
			// set TCP seq and adjust TCP flags
			seq := firstTCPSeqNum + uint32(i*gsoSize)
			binary.BigEndian.PutUint32(out[options.CsumStart+4:], seq)

			if payloadEnd != len(in) {
				// FIN and PSH should only be set on last segment. Last
				// specifically means last input segment, not last output, which
				// could be last fitting, but not last input.
				clearFlags := tcpFlagFIN | tcpFlagPSH
				out[options.CsumStart+tcpFlagsOffset] &^= clearFlags
			}
		} else {
			// set UDP header len
			udpLen := uint16(payloadSize) + options.HdrLen - options.CsumStart
			binary.BigEndian.PutUint16(out[options.CsumStart+4:], udpLen)
		}

		// transport checksum
		out[transportCsumAt], out[transportCsumAt+1] = 0, 0 // clear tcp/udp checksum
		transportHeaderLen := int(options.HdrLen - options.CsumStart)
		pseudoLen := uint16(transportHeaderLen + payloadSize)
		transportCSum := PseudoHeaderChecksum(
			protocol,
			in[srcAddrOffset:srcAddrOffset+addrLen],
			in[srcAddrOffset+addrLen:srcAddrOffset+2*addrLen],
			pseudoLen,
		)
		transportCSum = ^Checksum(
			out[options.CsumStart:packetSize],
			transportCSum,
		)
		binary.BigEndian.PutUint16(
			out[options.CsumStart+options.CsumOffset:],
			transportCSum,
		)
	}

	if n < totalOutSegments {
		return n, ErrTooManySegments
	}
	return n, nil
}
