/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2017-2023 WireGuard LLC. All Rights Reserved.
 */

package tun

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"slices"
	"unsafe"

	"github.com/tailscale/wireguard-go/conn"
	"golang.org/x/sys/unix"
)

// virtioNetHdr is defined in the kernel in include/uapi/linux/virtio_net.h. The
// kernel symbol is virtio_net_hdr.
type virtioNetHdr struct {
	flags      uint8
	gsoType    uint8
	hdrLen     uint16
	gsoSize    uint16
	csumStart  uint16
	csumOffset uint16
}

func (v *virtioNetHdr) toGSOOptions() (GSOOptions, error) {
	var gsoType GSOType
	switch v.gsoType {
	case unix.VIRTIO_NET_HDR_GSO_NONE:
		gsoType = GSONone
	case unix.VIRTIO_NET_HDR_GSO_TCPV4:
		gsoType = GSOTCPv4
	case unix.VIRTIO_NET_HDR_GSO_TCPV6:
		gsoType = GSOTCPv6
	case unix.VIRTIO_NET_HDR_GSO_UDP_L4:
		gsoType = GSOUDPL4
	default:
		return GSOOptions{}, fmt.Errorf("unsupported virtio gsoType: %d", v.gsoType)
	}
	return GSOOptions{
		GSOType:    gsoType,
		HdrLen:     v.hdrLen,
		CsumStart:  v.csumStart,
		CsumOffset: v.csumOffset,
		GSOSize:    v.gsoSize,
		NeedsCsum:  v.flags&unix.VIRTIO_NET_HDR_F_NEEDS_CSUM != 0,
	}, nil
}

func (v *virtioNetHdr) decode(b []byte) error {
	if len(b) < virtioNetHdrLen {
		return io.ErrShortBuffer
	}
	copy(unsafe.Slice((*byte)(unsafe.Pointer(v)), virtioNetHdrLen), b[:virtioNetHdrLen])
	return nil
}

func (v *virtioNetHdr) encode(b []byte) error {
	if len(b) < virtioNetHdrLen {
		return io.ErrShortBuffer
	}
	copy(b[:virtioNetHdrLen], unsafe.Slice((*byte)(unsafe.Pointer(v)), virtioNetHdrLen))
	return nil
}

const (
	// virtioNetHdrLen is the length in bytes of virtioNetHdr. This matches the
	// shape of the C ABI for its kernel counterpart -- sizeof(virtio_net_hdr).
	virtioNetHdrLen = int(unsafe.Sizeof(virtioNetHdr{}))

	// Vector layout: [virtioHdr | headPacket | coalescedPayloadFragments...]
	iovVirtioNetHdrIdx         = 0
	iovEmptyVectorLen          = 1
	iovHeadPacketIdx           = 1
	iovSinglePacketLen         = 2
	iovFirstPayloadFragmentIdx = 2

	maxScatterGatherFragments = 1024 // Limited by UIO_MAXIOV of 1024.
)

// groToWrite holds the write-ordered scatter-gather IO vectors for
// writev.
type groToWrite struct {
	// Each iov is a [][]byte:
	// - iovs[i][0] is the pre-allocated virtio header, fixed to [virtioNetHdrLen]
	// - iovs[i][1] is the head packet with transport headers
	// - iovs[i][2:] are coalesced payload fragments
	//
	// Here and elsewhere, "packet" refers to the full IP packet including transport headers,
	// and "payload fragment" refers to the data without transport headers.
	//
	// Empty iov is length 1 (iovEmptyVectorLen), with the first element (iovVirtioNetHdrIdx) zeroed.
	// Single packet iov is length 2 (iovSinglePacketLen),
	// with the first element zeroed, and the full packet at index 1 (iovHeadPacketIdx).
	// Coalesced packet is length >2, with the first element written from [virtioNetHdr],
	// the second element the head packet,
	// and the remaining elements (iovFirstPayloadFragmentIdx:) coalesced payload fragments.
	iovs      [][][]byte
	allocated int
}

func newGROToWrite() groToWrite {
	wi := groToWrite{
		iovs: make([][][]byte, 0, conn.IdealBatchSize),
	}
	for range cap(wi.iovs) {
		wi.appendIov(nil) // pre-allocate virtio headers
	}
	wi.iovs = wi.iovs[:0]
	return wi
}

// appendIov extends iovs by one, reusing the pre-allocated backing.
// Returns the index of the new item's [][]byte.
func (w *groToWrite) appendIov(pkt []byte) int {
	n := len(w.iovs)
	if n < w.allocated {
		w.iovs = w.iovs[:n+1]
	} else {
		iov := make([][]byte, 1, conn.IdealBatchSize)
		iov[iovVirtioNetHdrIdx] = make([]byte, virtioNetHdrLen)
		w.iovs = append(w.iovs, iov)
		w.allocated++
	}
	if pkt != nil {
		// Nil is passed only in [newGROToWrite] and tests.
		w.iovs[n] = append(w.iovs[n], pkt)
	}
	return n
}

func (w *groToWrite) reset() {
	for i := range w.iovs {
		clear(w.iovs[i][iovVirtioNetHdrIdx])
		w.iovs[i] = w.iovs[i][:iovEmptyVectorLen] // keep virtio header alloc
	}
	w.iovs = w.iovs[:0]
}

// tcpFlowKey represents the key for a TCP flow.
type tcpFlowKey struct {
	srcAddr, dstAddr [16]byte
	srcPort, dstPort uint16
	rxAck            uint32 // varying ack values should not be coalesced. Treat them as separate flows.
	isV6             bool
}

// tcpGROTable holds flow and coalescing information for the purposes of TCP GRO.
type tcpGROTable struct {
	itemsByFlow map[tcpFlowKey][]tcpGROItem
	itemsPool   [][]tcpGROItem
}

func newTCPGROTable() *tcpGROTable {
	t := &tcpGROTable{
		itemsByFlow: make(map[tcpFlowKey][]tcpGROItem, conn.IdealBatchSize),
		itemsPool:   make([][]tcpGROItem, conn.IdealBatchSize),
	}
	for i := range t.itemsPool {
		t.itemsPool[i] = make([]tcpGROItem, 0, conn.IdealBatchSize)
	}
	return t
}

func newTCPFlowKey(pkt []byte, srcAddrOffset, dstAddrOffset, tcphOffset int) tcpFlowKey {
	key := tcpFlowKey{}
	addrSize := dstAddrOffset - srcAddrOffset
	copy(key.srcAddr[:], pkt[srcAddrOffset:dstAddrOffset])
	copy(key.dstAddr[:], pkt[dstAddrOffset:dstAddrOffset+addrSize])
	key.srcPort = binary.BigEndian.Uint16(pkt[tcphOffset:])
	key.dstPort = binary.BigEndian.Uint16(pkt[tcphOffset+2:])
	key.rxAck = binary.BigEndian.Uint32(pkt[tcphOffset+8:])
	key.isV6 = addrSize == 16
	return key
}

// lookupOrInsert looks up a flow for the provided packet and metadata,
// returning the packets found for the flow, or inserting a new one if none
// is found.
func (t *tcpGROTable) lookupOrInsert(pkt []byte, srcAddrOffset, dstAddrOffset, tcphOffset, tcphLen int, wi *groToWrite) ([]tcpGROItem, bool) {
	key := newTCPFlowKey(pkt, srcAddrOffset, dstAddrOffset, tcphOffset)
	items, ok := t.itemsByFlow[key]
	if ok {
		return items, ok
	}
	// TODO: insert() performs another map lookup. This could be rearranged to avoid.
	t.insert(pkt, srcAddrOffset, dstAddrOffset, tcphOffset, tcphLen, wi)
	return nil, false
}

// insert an item in the table for the provided packet and packet metadata.
func (t *tcpGROTable) insert(pkt []byte, srcAddrOffset, dstAddrOffset, tcphOffset, tcphLen int, wi *groToWrite) {
	key := newTCPFlowKey(pkt, srcAddrOffset, dstAddrOffset, tcphOffset)
	idx := wi.appendIov(pkt)
	item := tcpGROItem{
		key:        key,
		outputIdx:  uint16(idx),
		gsoSize:    uint16(len(pkt[tcphOffset+tcphLen:])),
		iphLen:     uint8(tcphOffset),
		tcphLen:    uint8(tcphLen),
		sentSeq:    binary.BigEndian.Uint32(pkt[tcphOffset+4:]),
		pshSet:     pkt[tcphOffset+tcpFlagsOffset]&tcpFlagPSH != 0,
		payloadLen: uint16(len(pkt[tcphOffset+tcphLen:])),
	}
	items, ok := t.itemsByFlow[key]
	if !ok {
		items = t.newItems()
	}
	items = append(items, item)
	t.itemsByFlow[key] = items
}

func (t *tcpGROTable) updateAt(item tcpGROItem, i int) {
	items, _ := t.itemsByFlow[item.key]
	items[i] = item
}

func (t *tcpGROTable) deleteAt(key tcpFlowKey, i int) {
	items, _ := t.itemsByFlow[key]
	items = append(items[:i], items[i+1:]...)
	t.itemsByFlow[key] = items
}

// tcpGROItem represents bookkeeping data for a TCP packet during the lifetime
// of a GRO evaluation across a vector of packets.
type tcpGROItem struct {
	key        tcpFlowKey
	sentSeq    uint32 // the sequence number
	outputIdx  uint16 // index into groToWrite
	payloadLen uint16 // accumulated payload bytes
	gsoSize    uint16 // payload size
	iphLen     uint8  // ip header len
	tcphLen    uint8  // tcp header len
	pshSet     bool   // psh flag is set
}

func (t *tcpGROTable) newItems() []tcpGROItem {
	var items []tcpGROItem
	items, t.itemsPool = t.itemsPool[len(t.itemsPool)-1], t.itemsPool[:len(t.itemsPool)-1]
	return items
}

func (t *tcpGROTable) reset() {
	for k, items := range t.itemsByFlow {
		items = items[:0]
		t.itemsPool = append(t.itemsPool, items)
		delete(t.itemsByFlow, k)
	}
}

// udpFlowKey represents the key for a UDP flow.
type udpFlowKey struct {
	srcAddr, dstAddr [16]byte
	srcPort, dstPort uint16
	isV6             bool
}

// udpGROTable holds flow and coalescing information for the purposes of UDP GRO.
type udpGROTable struct {
	itemsByFlow map[udpFlowKey][]udpGROItem
	itemsPool   [][]udpGROItem
}

func newUDPGROTable() *udpGROTable {
	u := &udpGROTable{
		itemsByFlow: make(map[udpFlowKey][]udpGROItem, conn.IdealBatchSize),
		itemsPool:   make([][]udpGROItem, conn.IdealBatchSize),
	}
	for i := range u.itemsPool {
		u.itemsPool[i] = make([]udpGROItem, 0, conn.IdealBatchSize)
	}
	return u
}

func newUDPFlowKey(pkt []byte, srcAddrOffset, dstAddrOffset, udphOffset int) udpFlowKey {
	key := udpFlowKey{}
	addrSize := dstAddrOffset - srcAddrOffset
	copy(key.srcAddr[:], pkt[srcAddrOffset:dstAddrOffset])
	copy(key.dstAddr[:], pkt[dstAddrOffset:dstAddrOffset+addrSize])
	key.srcPort = binary.BigEndian.Uint16(pkt[udphOffset:])
	key.dstPort = binary.BigEndian.Uint16(pkt[udphOffset+2:])
	key.isV6 = addrSize == 16
	return key
}

// lookupOrInsert looks up a flow for the provided packet and metadata,
// returning the packets found for the flow, or inserting a new one if none
// is found.
func (u *udpGROTable) lookupOrInsert(pkt []byte, srcAddrOffset, dstAddrOffset, udphOffset int, wi *groToWrite) ([]udpGROItem, bool) {
	key := newUDPFlowKey(pkt, srcAddrOffset, dstAddrOffset, udphOffset)
	items, ok := u.itemsByFlow[key]
	if ok {
		return items, ok
	}
	// TODO: insert() performs another map lookup. This could be rearranged to avoid.
	u.insert(pkt, srcAddrOffset, dstAddrOffset, udphOffset, wi, false)
	return nil, false
}

// insert an item in the table for the provided packet and packet metadata.
func (u *udpGROTable) insert(pkt []byte, srcAddrOffset, dstAddrOffset, udphOffset int, wi *groToWrite, cSumKnownInvalid bool) {
	key := newUDPFlowKey(pkt, srcAddrOffset, dstAddrOffset, udphOffset)
	idx := wi.appendIov(pkt)
	item := udpGROItem{
		key:              key,
		outputIdx:        uint16(idx),
		gsoSize:          uint16(len(pkt[udphOffset+udphLen:])),
		iphLen:           uint8(udphOffset),
		cSumKnownInvalid: cSumKnownInvalid,
		payloadLen:       uint16(len(pkt[udphOffset+udphLen:])),
	}
	items, ok := u.itemsByFlow[key]
	if !ok {
		items = u.newItems()
	}
	items = append(items, item)
	u.itemsByFlow[key] = items
}

func (u *udpGROTable) updateAt(item udpGROItem, i int) {
	items, _ := u.itemsByFlow[item.key]
	items[i] = item
}

// udpGROItem represents bookkeeping data for a UDP packet during the lifetime
// of a GRO evaluation across a vector of packets.
type udpGROItem struct {
	key              udpFlowKey
	outputIdx        uint16 // index into groToWrite
	payloadLen       uint16 // accumulated payload bytes
	gsoSize          uint16 // payload size
	iphLen           uint8  // ip header len
	cSumKnownInvalid bool   // UDP header checksum validity; a false value DOES NOT imply valid, just unknown.
}

func (u *udpGROTable) newItems() []udpGROItem {
	var items []udpGROItem
	items, u.itemsPool = u.itemsPool[len(u.itemsPool)-1], u.itemsPool[:len(u.itemsPool)-1]
	return items
}

func (u *udpGROTable) reset() {
	for k, items := range u.itemsByFlow {
		items = items[:0]
		u.itemsPool = append(u.itemsPool, items)
		delete(u.itemsByFlow, k)
	}
}

// canCoalesce represents the outcome of checking if two TCP packets are
// candidates for coalescing.
type canCoalesce int

const (
	coalescePrepend     canCoalesce = -1
	coalesceUnavailable canCoalesce = 0
	coalesceAppend      canCoalesce = 1
)

// ipHeadersCanCoalesce returns true if the IP headers found in pktA and pktB
// meet all requirements to be merged as part of a GRO operation, otherwise it
// returns false.
func ipHeadersCanCoalesce(pktA, pktB []byte) bool {
	if len(pktA) < 9 || len(pktB) < 9 {
		return false
	}
	if pktA[0]>>4 == 6 {
		if pktA[0] != pktB[0] || pktA[1]>>4 != pktB[1]>>4 {
			// cannot coalesce with unequal Traffic class values
			return false
		}
		if pktA[7] != pktB[7] {
			// cannot coalesce with unequal Hop limit values
			return false
		}
	} else {
		if pktA[1] != pktB[1] {
			// cannot coalesce with unequal ToS values
			return false
		}
		if pktA[6]>>5 != pktB[6]>>5 {
			// cannot coalesce with unequal DF or reserved bits. MF is checked
			// further up the stack.
			return false
		}
		if pktA[8] != pktB[8] {
			// cannot coalesce with unequal TTL values
			return false
		}
	}
	return true
}

// udpPacketsCanCoalesce evaluates if pkt can be coalesced with the packet
// described by item. iphLen and gsoSize describe pkt.
func udpPacketsCanCoalesce(pkt []byte, gsoSize uint16, item udpGROItem, wi *groToWrite) canCoalesce {
	if len(wi.iovs[item.outputIdx]) >= maxScatterGatherFragments {
		return coalesceUnavailable
	}
	headPacket := wi.iovs[item.outputIdx][iovHeadPacketIdx]
	if !ipHeadersCanCoalesce(pkt, headPacket) {
		return coalesceUnavailable
	}
	if item.payloadLen%item.gsoSize != 0 {
		// A smaller than gsoSize packet has been appended previously.
		// Nothing can come after a smaller packet on the end.
		return coalesceUnavailable
	}
	if gsoSize > item.gsoSize {
		// We cannot have a larger packet following a smaller one.
		return coalesceUnavailable
	}
	if int(item.iphLen)+udphLen+int(item.payloadLen)+int(gsoSize) > maxUint16 {
		return coalesceUnavailable
	}
	return coalesceAppend
}

// tcpPacketsCanCoalesce evaluates if pkt can be coalesced with the packet
// described by item. This function makes considerations that match the kernel's
// GRO self tests, which can be found in tools/testing/selftests/net/gro.c.
func tcpPacketsCanCoalesce(pkt []byte, iphLen, tcphLen uint8, seq uint32, pshSet bool, gsoSize uint16, item tcpGROItem, wi *groToWrite) canCoalesce {
	if len(wi.iovs[item.outputIdx]) >= maxScatterGatherFragments {
		return coalesceUnavailable
	}
	headPacket := wi.iovs[item.outputIdx][iovHeadPacketIdx]
	if tcphLen != item.tcphLen {
		// cannot coalesce with unequal tcp options len
		return coalesceUnavailable
	}
	if tcphLen > 20 {
		if !bytes.Equal(pkt[iphLen+20:iphLen+tcphLen], headPacket[item.iphLen+20:iphLen+tcphLen]) {
			// cannot coalesce with unequal tcp options
			return coalesceUnavailable
		}
	}
	if !ipHeadersCanCoalesce(pkt, headPacket) {
		return coalesceUnavailable
	}
	if int(item.iphLen)+int(item.tcphLen)+int(item.payloadLen)+int(gsoSize) > maxUint16 {
		return coalesceUnavailable
	}
	// seq adjacency
	if seq == item.sentSeq+uint32(item.payloadLen) { // pkt aligns following item from a seq num perspective
		if item.pshSet {
			// We cannot append to a segment that has the PSH flag set, PSH
			// can only be set on the final segment in a reassembled group.
			return coalesceUnavailable
		}
		if item.payloadLen%item.gsoSize != 0 {
			// A smaller than gsoSize packet has been appended previously.
			// Nothing can come after a smaller packet on the end.
			return coalesceUnavailable
		}
		if gsoSize > item.gsoSize {
			// We cannot have a larger packet following a smaller one.
			return coalesceUnavailable
		}
		return coalesceAppend
	} else if seq+uint32(gsoSize) == item.sentSeq { // pkt aligns in front of item from a seq num perspective
		if pshSet {
			// We cannot prepend with a segment that has the PSH flag set, PSH
			// can only be set on the final segment in a reassembled group.
			return coalesceUnavailable
		}
		if gsoSize < item.gsoSize {
			// We cannot have a larger packet following a smaller one.
			return coalesceUnavailable
		}
		if gsoSize > item.gsoSize && len(wi.iovs[item.outputIdx]) > iovSinglePacketLen {
			// There's at least one previous merge, and we're larger than all
			// previous. This would put multiple smaller packets on the end.
			return coalesceUnavailable
		}
		return coalescePrepend
	}
	return coalesceUnavailable
}

func checksumValid(pkt []byte, iphLen, proto uint8, isV6 bool) bool {
	srcAddrAt := ipv4SrcAddrOffset
	addrSize := 4
	if isV6 {
		srcAddrAt = ipv6SrcAddrOffset
		addrSize = 16
	}
	lenForPseudo := uint16(len(pkt) - int(iphLen))
	cSum := PseudoHeaderChecksum(proto, pkt[srcAddrAt:srcAddrAt+addrSize], pkt[srcAddrAt+addrSize:srcAddrAt+addrSize*2], lenForPseudo)
	return ^Checksum(pkt[iphLen:], cSum) == 0
}

// coalesceResult represents the result of attempting to coalesce two packets.
type coalesceResult int

const (
	coalescePSHEnding coalesceResult = iota
	coalesceItemInvalidCSum
	coalescePktInvalidCSum
	coalesceSuccess
)

// coalesceUDPPackets attempts to coalesce pkt with the packet described by
// item, and returns the outcome.
func coalesceUDPPackets(pkt []byte, item *udpGROItem, wi *groToWrite, isV6 bool) coalesceResult {
	headersLen := int(item.iphLen) + udphLen
	iov := &wi.iovs[item.outputIdx]
	if len(*iov) == iovSinglePacketLen {
		if item.cSumKnownInvalid || !checksumValid((*iov)[iovHeadPacketIdx], item.iphLen, unix.IPPROTO_UDP, isV6) {
			return coalesceItemInvalidCSum
		}
	}
	if !checksumValid(pkt, item.iphLen, unix.IPPROTO_UDP, isV6) {
		return coalescePktInvalidCSum
	}
	*iov = append(*iov, pkt[headersLen:])
	item.payloadLen += uint16(len(pkt) - headersLen)
	return coalesceSuccess
}

// coalesceTCPPackets attempts to coalesce pkt with the packet described by
// item, and returns the outcome.
func coalesceTCPPackets(mode canCoalesce, pkt []byte, gsoSize uint16, seq uint32, pshSet bool, item *tcpGROItem, wi *groToWrite, isV6 bool) coalesceResult {
	headersLen := int(item.iphLen) + int(item.tcphLen)
	iov := &wi.iovs[item.outputIdx]
	if mode == coalescePrepend {
		if pshSet {
			return coalescePSHEnding
		}
		if len(*iov) == iovSinglePacketLen {
			if !checksumValid((*iov)[iovHeadPacketIdx], item.iphLen, unix.IPPROTO_TCP, isV6) {
				return coalesceItemInvalidCSum
			}
		}
		if !checksumValid(pkt, item.iphLen, unix.IPPROTO_TCP, isV6) {
			return coalescePktInvalidCSum
		}
		item.sentSeq = seq
		oldHead := (*iov)[iovHeadPacketIdx]
		(*iov)[iovHeadPacketIdx] = pkt
		oldHeadPayload := oldHead[headersLen:]
		if len(oldHeadPayload) > 0 {
			*iov = slices.Insert(*iov, iovFirstPayloadFragmentIdx, oldHeadPayload)
		}
	} else {
		if len(*iov) == iovSinglePacketLen {
			if !checksumValid((*iov)[iovHeadPacketIdx], item.iphLen, unix.IPPROTO_TCP, isV6) {
				return coalesceItemInvalidCSum
			}
		}
		if !checksumValid(pkt, item.iphLen, unix.IPPROTO_TCP, isV6) {
			return coalescePktInvalidCSum
		}
		if pshSet {
			// We are appending a segment with PSH set.
			item.pshSet = pshSet
			(*iov)[iovHeadPacketIdx][item.iphLen+tcpFlagsOffset] |= tcpFlagPSH
		}
		*iov = append(*iov, pkt[headersLen:])
	}

	if gsoSize > item.gsoSize {
		item.gsoSize = gsoSize
	}

	item.payloadLen += uint16(len(pkt) - headersLen)
	return coalesceSuccess
}

const (
	ipv4FlagMoreFragments uint8 = 0x20
)

const (
	maxUint16 = 1<<16 - 1
)

type groResult int

const (
	groResultNoop groResult = iota
	groResultTableInsert
	groResultCoalesced
)

// tcpGRO evaluates the TCP packet for coalescing with
// existing packets tracked in table. It returns a groResultNoop when no
// action was taken, groResultTableInsert when the evaluated packet was
// inserted into table, and groResultCoalesced when the evaluated packet was
// coalesced with another packet in table.
func tcpGRO(pkt []byte, table *tcpGROTable, wi *groToWrite, isV6 bool) groResult {
	if len(pkt) > maxUint16 {
		// A valid IPv4 or IPv6 packet will never exceed this.
		return groResultNoop
	}
	iphLen := int((pkt[0] & 0x0F) * 4)
	if isV6 {
		iphLen = 40
		ipv6HPayloadLen := int(binary.BigEndian.Uint16(pkt[4:]))
		if ipv6HPayloadLen != len(pkt)-iphLen {
			return groResultNoop
		}
	} else {
		totalLen := int(binary.BigEndian.Uint16(pkt[2:]))
		if totalLen != len(pkt) {
			return groResultNoop
		}
	}
	if len(pkt) < iphLen {
		return groResultNoop
	}
	tcphLen := int((pkt[iphLen+12] >> 4) * 4)
	if tcphLen < 20 || tcphLen > 60 {
		return groResultNoop
	}
	if len(pkt) < iphLen+tcphLen {
		return groResultNoop
	}
	if !isV6 {
		if pkt[6]&ipv4FlagMoreFragments != 0 || pkt[6]<<3 != 0 || pkt[7] != 0 {
			// no GRO support for fragmented segments for now
			return groResultNoop
		}
	}
	tcpFlags := pkt[iphLen+tcpFlagsOffset]
	var pshSet bool
	// not a candidate if any non-ACK flags (except PSH+ACK) are set
	if tcpFlags != tcpFlagACK {
		if pkt[iphLen+tcpFlagsOffset] != tcpFlagACK|tcpFlagPSH {
			return groResultNoop
		}
		pshSet = true
	}
	gsoSize := uint16(len(pkt) - tcphLen - iphLen)
	// not a candidate if payload len is 0
	if gsoSize < 1 {
		return groResultNoop
	}
	seq := binary.BigEndian.Uint32(pkt[iphLen+4:])
	srcAddrOffset := ipv4SrcAddrOffset
	addrLen := 4
	if isV6 {
		srcAddrOffset = ipv6SrcAddrOffset
		addrLen = 16
	}
	items, existing := table.lookupOrInsert(pkt, srcAddrOffset, srcAddrOffset+addrLen, iphLen, tcphLen, wi)
	if !existing {
		return groResultTableInsert
	}
	for i := len(items) - 1; i >= 0; i-- {
		// In the best case of packets arriving in order iterating in reverse is
		// more efficient if there are multiple items for a given flow. This
		// also enables a natural table.deleteAt() in the
		// coalesceItemInvalidCSum case without the need for index tracking.
		// This algorithm makes a best effort to coalesce in the event of
		// unordered packets, where pkt may land anywhere in items from a
		// sequence number perspective, however once an item is inserted into
		// the table it is never compared across other items later.
		item := items[i]
		can := tcpPacketsCanCoalesce(pkt, uint8(iphLen), uint8(tcphLen), seq, pshSet, gsoSize, item, wi)
		if can != coalesceUnavailable {
			result := coalesceTCPPackets(can, pkt, gsoSize, seq, pshSet, &item, wi, isV6)
			switch result {
			case coalesceSuccess:
				table.updateAt(item, i)
				return groResultCoalesced
			case coalesceItemInvalidCSum:
				// delete the item with an invalid csum
				table.deleteAt(item.key, i)
			case coalescePktInvalidCSum:
				// no point in inserting an item that we can't coalesce
				return groResultNoop
			default:
			}
		}
	}
	// failed to coalesce with any other packets; store the item in the flow
	table.insert(pkt, srcAddrOffset, srcAddrOffset+addrLen, iphLen, tcphLen, wi)
	return groResultTableInsert
}

// applyTCPCoalesceAccounting updates headers to account for coalescing based on the
// metadata found in table.
func applyTCPCoalesceAccounting(wi *groToWrite, table *tcpGROTable) error {
	for _, items := range table.itemsByFlow {
		for _, item := range items {
			iov := wi.iovs[item.outputIdx]
			pkt := iov[iovHeadPacketIdx]
			if len(iov) > iovSinglePacketLen {
				totalLen := uint16(item.iphLen) + uint16(item.tcphLen) + item.payloadLen
				hdr := virtioNetHdr{
					flags:      unix.VIRTIO_NET_HDR_F_NEEDS_CSUM, // this turns into CHECKSUM_PARTIAL in the skb
					hdrLen:     uint16(item.iphLen + item.tcphLen),
					gsoSize:    item.gsoSize,
					csumStart:  uint16(item.iphLen),
					csumOffset: 16,
				}

				// Recalculate the total len (IPv4) or payload len (IPv6).
				// Recalculate the (IPv4) header checksum.
				if item.key.isV6 {
					hdr.gsoType = unix.VIRTIO_NET_HDR_GSO_TCPV6
					binary.BigEndian.PutUint16(pkt[4:], totalLen-uint16(item.iphLen)) // set new IPv6 header payload len
				} else {
					hdr.gsoType = unix.VIRTIO_NET_HDR_GSO_TCPV4
					pkt[10], pkt[11] = 0, 0
					binary.BigEndian.PutUint16(pkt[2:], totalLen) // set new total length
					iphCSum := ^Checksum(pkt[:item.iphLen], 0)    // compute IPv4 header checksum
					binary.BigEndian.PutUint16(pkt[10:], iphCSum) // set IPv4 header checksum field
				}
				err := hdr.encode(iov[iovVirtioNetHdrIdx])
				if err != nil {
					return err
				}

				// Calculate the pseudo header checksum and place it at the TCP
				// checksum offset. Downstream checksum offloading will combine
				// this with computation of the tcp header and payload checksum.
				addrLen := 4
				addrOffset := ipv4SrcAddrOffset
				if item.key.isV6 {
					addrLen = 16
					addrOffset = ipv6SrcAddrOffset
				}
				srcAddr := pkt[addrOffset : addrOffset+addrLen]
				dstAddr := pkt[addrOffset+addrLen : addrOffset+addrLen*2]
				psum := PseudoHeaderChecksum(unix.IPPROTO_TCP, srcAddr, dstAddr, totalLen-uint16(item.iphLen))
				binary.BigEndian.PutUint16(pkt[hdr.csumStart+hdr.csumOffset:], Checksum([]byte{}, psum))
			}
		}
	}
	return nil
}

// applyUDPCoalesceAccounting updates headers to account for coalescing based on the
// metadata found in table.
func applyUDPCoalesceAccounting(wi *groToWrite, table *udpGROTable) error {
	for _, items := range table.itemsByFlow {
		for _, item := range items {
			iov := wi.iovs[item.outputIdx]
			pkt := iov[iovHeadPacketIdx]
			if len(iov) > iovSinglePacketLen {
				totalLen := uint16(item.iphLen) + udphLen + item.payloadLen
				hdr := virtioNetHdr{
					flags:      unix.VIRTIO_NET_HDR_F_NEEDS_CSUM, // this turns into CHECKSUM_PARTIAL in the skb
					hdrLen:     uint16(item.iphLen) + udphLen,
					gsoSize:    item.gsoSize,
					csumStart:  uint16(item.iphLen),
					csumOffset: 6,
				}

				// Recalculate the total len (IPv4) or payload len (IPv6).
				// Recalculate the (IPv4) header checksum.
				hdr.gsoType = unix.VIRTIO_NET_HDR_GSO_UDP_L4
				if item.key.isV6 {
					binary.BigEndian.PutUint16(pkt[4:], totalLen-uint16(item.iphLen)) // set new IPv6 header payload len
				} else {
					pkt[10], pkt[11] = 0, 0
					binary.BigEndian.PutUint16(pkt[2:], totalLen) // set new total length
					iphCSum := ^Checksum(pkt[:item.iphLen], 0)    // compute IPv4 header checksum
					binary.BigEndian.PutUint16(pkt[10:], iphCSum) // set IPv4 header checksum field
				}
				err := hdr.encode(iov[iovVirtioNetHdrIdx])
				if err != nil {
					return err
				}

				// Recalculate the UDP len field value
				binary.BigEndian.PutUint16(pkt[item.iphLen+4:], udphLen+item.payloadLen)

				// Calculate the pseudo header checksum and place it at the UDP
				// checksum offset. Downstream checksum offloading will combine
				// this with computation of the udp header and payload checksum.
				addrLen := 4
				addrOffset := ipv4SrcAddrOffset
				if item.key.isV6 {
					addrLen = 16
					addrOffset = ipv6SrcAddrOffset
				}
				srcAddr := pkt[addrOffset : addrOffset+addrLen]
				dstAddr := pkt[addrOffset+addrLen : addrOffset+addrLen*2]
				psum := PseudoHeaderChecksum(unix.IPPROTO_UDP, srcAddr, dstAddr, totalLen-uint16(item.iphLen))
				binary.BigEndian.PutUint16(pkt[hdr.csumStart+hdr.csumOffset:], Checksum([]byte{}, psum))
			}
		}
	}
	return nil
}

type groCandidateType uint8

const (
	notGROCandidate groCandidateType = iota
	tcp4GROCandidate
	tcp6GROCandidate
	udp4GROCandidate
	udp6GROCandidate
)

func packetIsGROCandidate(b []byte, gro groDisablementFlags) groCandidateType {
	if len(b) < 28 {
		return notGROCandidate
	}
	if b[0]>>4 == 4 {
		if b[0]&0x0F != 5 {
			// IPv4 packets w/IP options do not coalesce
			return notGROCandidate
		}
		if b[9] == unix.IPPROTO_TCP && len(b) >= 40 && gro.canTCPGRO() {
			return tcp4GROCandidate
		}
		if b[9] == unix.IPPROTO_UDP && gro.canUDPGRO() {
			return udp4GROCandidate
		}
	} else if b[0]>>4 == 6 {
		if b[6] == unix.IPPROTO_TCP && len(b) >= 60 && gro.canTCPGRO() {
			return tcp6GROCandidate
		}
		if b[6] == unix.IPPROTO_UDP && len(b) >= 48 && gro.canUDPGRO() {
			return udp6GROCandidate
		}
	}
	return notGROCandidate
}

const (
	udphLen = 8
)

// udpGRO evaluates the UDP packet for coalescing with
// existing packets tracked in table. It returns a groResultNoop when no
// action was taken, groResultTableInsert when the evaluated packet was
// inserted into table, and groResultCoalesced when the evaluated packet was
// coalesced with another packet in table.
func udpGRO(pkt []byte, table *udpGROTable, wi *groToWrite, isV6 bool) groResult {
	if len(pkt) > maxUint16 {
		// A valid IPv4 or IPv6 packet will never exceed this.
		return groResultNoop
	}
	iphLen := int((pkt[0] & 0x0F) * 4)
	if isV6 {
		iphLen = 40
		ipv6HPayloadLen := int(binary.BigEndian.Uint16(pkt[4:]))
		if ipv6HPayloadLen != len(pkt)-iphLen {
			return groResultNoop
		}
	} else {
		totalLen := int(binary.BigEndian.Uint16(pkt[2:]))
		if totalLen != len(pkt) {
			return groResultNoop
		}
	}
	if len(pkt) < iphLen {
		return groResultNoop
	}
	if len(pkt) < iphLen+udphLen {
		return groResultNoop
	}
	if !isV6 {
		if pkt[6]&ipv4FlagMoreFragments != 0 || pkt[6]<<3 != 0 || pkt[7] != 0 {
			// no GRO support for fragmented segments for now
			return groResultNoop
		}
	}
	gsoSize := uint16(len(pkt) - udphLen - iphLen)
	// not a candidate if payload len is 0
	if gsoSize < 1 {
		return groResultNoop
	}
	srcAddrOffset := ipv4SrcAddrOffset
	addrLen := 4
	if isV6 {
		srcAddrOffset = ipv6SrcAddrOffset
		addrLen = 16
	}
	items, existing := table.lookupOrInsert(pkt, srcAddrOffset, srcAddrOffset+addrLen, iphLen, wi)
	if !existing {
		return groResultTableInsert
	}
	// With UDP we only check the last item, otherwise we could reorder packets
	// for a given flow. We must also always insert a new item, or successfully
	// coalesce with an existing item, for the same reason.
	item := items[len(items)-1]
	can := udpPacketsCanCoalesce(pkt, gsoSize, item, wi)
	var pktCSumKnownInvalid bool
	if can == coalesceAppend {
		result := coalesceUDPPackets(pkt, &item, wi, isV6)
		switch result {
		case coalesceSuccess:
			table.updateAt(item, len(items)-1)
			return groResultCoalesced
		case coalesceItemInvalidCSum:
			// If the existing item has an invalid csum we take no action. A new
			// item will be stored after it, and the existing item will never be
			// revisited as part of future coalescing candidacy checks.
		case coalescePktInvalidCSum:
			// We must insert a new item, but we also mark it as invalid csum
			// to prevent a repeat checksum validation.
			pktCSumKnownInvalid = true
		default:
		}
	}
	// failed to coalesce with any other packets; store the item in the flow
	table.insert(pkt, srcAddrOffset, srcAddrOffset+addrLen, iphLen, wi, pktCSumKnownInvalid)
	return groResultTableInsert
}

// handleGRO evaluates bufs for GRO, and populates wi with the resulting
// io vectors for writev. wi, tcpTable, and udpTable should initially be
// empty (but non-nil), and are passed in to save allocs as the caller may reset
// and recycle them across vectors of packets. gro indicates if TCP and UDP GRO
// are supported/enabled.
func handleGRO(bufs [][]byte, offset int, tcpTable *tcpGROTable, udpTable *udpGROTable, gro groDisablementFlags, wi *groToWrite) error {
	for i := range bufs {
		if offset < virtioNetHdrLen || offset > len(bufs[i])-1 {
			return errors.New("invalid offset")
		}
		pkt := bufs[i][offset:]
		var result groResult
		switch packetIsGROCandidate(pkt, gro) {
		case tcp4GROCandidate:
			result = tcpGRO(pkt, tcpTable, wi, false)
		case tcp6GROCandidate:
			result = tcpGRO(pkt, tcpTable, wi, true)
		case udp4GROCandidate:
			result = udpGRO(pkt, udpTable, wi, false)
		case udp6GROCandidate:
			result = udpGRO(pkt, udpTable, wi, true)
		}
		switch result {
		case groResultNoop:
			wi.appendIov(pkt)
		case groResultTableInsert:
			// already in wi via table insert
		}
	}
	errTCP := applyTCPCoalesceAccounting(wi, tcpTable)
	errUDP := applyUDPCoalesceAccounting(wi, udpTable)
	return errors.Join(errTCP, errUDP)
}
