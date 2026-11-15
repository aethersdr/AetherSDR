/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2017-2023 WireGuard LLC. All Rights Reserved.
 */

package device

import (
	"encoding/binary"
	"errors"
	"fmt"
	"net"
	"net/netip"
	"os"
	"sync"
	"time"

	"github.com/tailscale/wireguard-go/conn"
	"github.com/tailscale/wireguard-go/tun"
	"golang.org/x/crypto/chacha20poly1305"
	"golang.org/x/net/ipv4"
	"golang.org/x/net/ipv6"
)

/* Outbound flow
 *
 * 1. TUN queue
 * 2. Routing (sequential)
 * 3. Nonce assignment (sequential)
 * 4. Encryption (parallel)
 * 5. Transmission (sequential)
 *
 * The functions in this file occur (roughly) in the order in
 * which the packets are processed.
 *
 * Locking, Producers and Consumers
 *
 * The order of packets (per peer) must be maintained,
 * but encryption of packets happen out-of-order:
 *
 * The sequential consumers will attempt to take the lock,
 * workers release lock when they have completed work (encryption) on the packet.
 *
 * If the element is inserted into the "encryption queue",
 * the content is preceded by enough "junk" to contain the transport header
 * (to allow the construction of transport messages in-place)
 */

type QueueOutboundElement struct {
	buffer *packetBuf // shared packet buffer that contains packet
	packet []byte     // packet is always a slice of buffer.slab
	// plaintextOffset describes the pre-encryption offset of packet within
	// buffer.slab. The encryption process grows packet on both ends to make
	// room for any potential encapsulating transport, WireGuard header, and
	// tail padding.
	plaintextOffset int
	nonce           uint64   // nonce for encryption
	keypair         *Keypair // keypair for encryption
	peer            *Peer    // related peer
}

type QueueOutboundElementsContainer struct {
	// filling is a one-shot barrier signaling encryption→send handoff.
	// SendStagedPackets calls Add(1) before sending the container down
	// the encryption and outbound queues; RoutineEncryption calls Done
	// after encrypting; RoutineSequentialSender calls Wait before
	// reading the encrypted packets.
	filling sync.WaitGroup
	elems   []*QueueOutboundElement
}

// clearPointers clears elem fields that contain pointers.
// This makes the garbage collector's life easier and
// avoids accidentally keeping other objects around unnecessarily.
// It also reduces the possible collateral damage from use-after-free bugs.
func (elem *QueueOutboundElement) clearPointers() {
	elem.buffer = nil
	elem.packet = nil
	elem.keypair = nil
	elem.peer = nil
}

// SendKeepalive queues a keepalive if no packets are queued for
// peer.
func (peer *Peer) SendKeepalive() {
	if len(peer.queue.staged) == 0 {
		elem := peer.device.GetOutboundElement()
		elem.buffer = peer.device.getSmallPacketBuf()
		elem.plaintextOffset = MessageEncapsulatingTransportSize + MessageTransportHeaderSize
		elemsContainer := peer.device.GetOutboundElementsContainer()
		elemsContainer.elems = append(elemsContainer.elems, elem)
		if queued := peer.doIfRunning(func() {
			select {
			case peer.queue.staged <- elemsContainer:
				peer.device.log.Verbosef("%v - Sending keepalive packet", peer)
			default:
				peer.device.PutOutboundElement(elem)
				peer.device.PutOutboundElementsContainer(elemsContainer)
			}
		}); !queued {
			peer.device.PutOutboundElement(elem)
			peer.device.PutOutboundElementsContainer(elemsContainer)
		}
	}
	peer.SendStagedPackets()
}

// SendPriorityMessage invokes the [PeerPriorityMessageFunc] callback if one is
// set, and queues the returned message for encryption and transmission if the
// current keypair is valid.
func (peer *Peer) SendPriorityMessage() {
	f := peer.device.priorityMsgFn.Load()
	if f == nil {
		return
	}
	keypair := peer.keypairs.Current()
	if keypair == nil || keypair.sendNonce.Load() >= RejectAfterMessages || time.Since(keypair.created) >= RejectAfterTime {
		// SendStagedPackets initializes a handshake when the keypair is invalid,
		// but we explicitly avoid that here. A priority message is only intended
		// to flow around symmetric session establishment, but it should never
		// trigger a new session. Reaching this branch due to nonce exhaustion
		// or keypair expiration is highly unlikely considering where
		// SendPriorityMessage is called (at current keypair establishment).
		return
	}

	// get plaintext message to send
	msg := (*f)(peer.handshake.remoteStatic)
	if len(msg) == 0 {
		return
	}
	if len(msg) > MaxPriorityMessageContentSize {
		peer.device.log.Verbosef("%v - Failed to queue priority message due to size", peer)
		return
	}

	// get pooled elements
	elem := peer.device.GetOutboundElement()
	elemsContainer := peer.device.GetOutboundElementsContainer()
	elemsContainer.elems = append(elemsContainer.elems, elem)
	buf := peer.device.getSmallPacketBuf()

	// initialize outbound element
	const offset = MessageEncapsulatingTransportSize + MessageTransportHeaderSize

	n := copy(buf.slab[offset:], msg)
	elem.buffer = buf
	elem.packet = buf.slab[offset : offset+n]
	elem.plaintextOffset = offset
	elem.peer = peer
	elem.nonce = keypair.sendNonce.Add(1) - 1
	if elem.nonce >= RejectAfterMessages {
		keypair.sendNonce.Store(RejectAfterMessages)
		peer.device.PutOutboundElement(elem)
		peer.device.PutOutboundElementsContainer(elemsContainer)
		return
	}
	elem.keypair = keypair

	// add to parallel and sequential queue
	peer.queueOutboundIfRunning(elemsContainer)
}

func (peer *Peer) SendHandshakeInitiation(isRetry bool) error {
	if !isRetry {
		peer.timers.handshakeAttempts.Store(0)
	}

	peer.handshake.mutex.RLock()
	if time.Since(peer.handshake.lastSentHandshake) < RekeyTimeout {
		peer.handshake.mutex.RUnlock()
		return nil
	}
	peer.handshake.mutex.RUnlock()

	peer.handshake.mutex.Lock()
	if time.Since(peer.handshake.lastSentHandshake) < RekeyTimeout {
		peer.handshake.mutex.Unlock()
		return nil
	}
	// handshakeOnUserSend is flipped false after lastSentHandshake checks,
	// enabling eventual transmission at a future call of this method, while
	// still honoring [RekeyTimeout].
	peer.handshakeOnUserSend.Store(false)
	peer.handshake.lastSentHandshake = time.Now()
	peer.handshake.mutex.Unlock()

	peer.device.log.Verbosef("%v - Sending handshake initiation", peer)

	msg, err := peer.device.CreateMessageInitiation(peer)
	if err != nil {
		peer.device.log.Errorf("%v - Failed to create initiation message: %v", peer, err)
		return err
	}

	buf := make([]byte, MessageEncapsulatingTransportSize+MessageInitiationSize)
	packet := buf[MessageEncapsulatingTransportSize:]
	_ = msg.marshal(packet)
	peer.cookieGenerator.AddMacs(packet)

	peer.timersAnyAuthenticatedPacketTraversal()
	peer.timersAnyAuthenticatedPacketSent()

	if isRetry {
		peer.device.config.metrics.MessageInitiationTXAttemptRetry.Add(1)
	} else {
		peer.device.config.metrics.MessageInitiationTXAttemptInitial.Add(1)
	}
	err = peer.SendBuffers([][]byte{buf})
	if err != nil {
		peer.device.log.Errorf("%v - Failed to send handshake initiation: %v", peer, err)
	}
	peer.timersHandshakeInitiated()

	return err
}

func (peer *Peer) SendHandshakeResponse() error {
	peer.handshake.mutex.Lock()
	peer.handshake.lastSentHandshake = time.Now()
	peer.handshake.mutex.Unlock()

	peer.device.log.Verbosef("%v - Sending handshake response", peer)

	response, err := peer.device.CreateMessageResponse(peer)
	if err != nil {
		peer.device.log.Errorf("%v - Failed to create response message: %v", peer, err)
		return err
	}

	buf := make([]byte, MessageEncapsulatingTransportSize+MessageResponseSize)
	packet := buf[MessageEncapsulatingTransportSize:]
	_ = response.marshal(packet)
	peer.cookieGenerator.AddMacs(packet)

	err = peer.BeginSymmetricSession()
	if err != nil {
		peer.device.log.Errorf("%v - Failed to derive keypair: %v", peer, err)
		return err
	}

	peer.timersSessionDerived()
	peer.timersAnyAuthenticatedPacketTraversal()
	peer.timersAnyAuthenticatedPacketSent()

	peer.device.config.metrics.MessageResponseTXAttempt.Add(1)
	// TODO: allocation could be avoided
	err = peer.SendBuffers([][]byte{buf})
	if err != nil {
		peer.device.log.Errorf("%v - Failed to send handshake response: %v", peer, err)
	}
	return err
}

func (device *Device) SendHandshakeCookie(initiatingElem *QueueHandshakeElement) error {
	device.log.Verbosef("Sending cookie response for denied handshake message for %v", initiatingElem.packetMeta.Endpoint.DstToString())

	sender := binary.LittleEndian.Uint32(initiatingElem.packet[4:8])
	reply, err := device.cookieChecker.CreateReply(initiatingElem.packet, sender, initiatingElem.packetMeta.Endpoint.DstToBytes())
	if err != nil {
		device.log.Errorf("Failed to create cookie reply: %v", err)
		return err
	}

	buf := make([]byte, MessageEncapsulatingTransportSize+MessageCookieReplySize)
	packet := buf[MessageEncapsulatingTransportSize:]
	_ = reply.marshal(packet)
	device.config.metrics.MessageCookieReplyTXAttempt.Add(1)
	// TODO: allocation could be avoided
	device.net.bind.Send([][]byte{buf}, initiatingElem.packetMeta.Endpoint, MessageEncapsulatingTransportSize)

	return nil
}

func (peer *Peer) keepKeyFreshSending() {
	keypair := peer.keypairs.Current()
	if keypair == nil {
		return
	}
	txHandshake := peer.handshakeOnUserSend.Load()
	nonce := keypair.sendNonce.Load()
	if txHandshake || nonce > RekeyAfterMessages || (keypair.isInitiator && time.Since(keypair.created) > RekeyAfterTime) {
		peer.SendHandshakeInitiation(false)
	}
}

// copyPacketBufIfFits copies the prefix of src.slab ending at the final packet
// into dst.slab if that prefix+[outboundPlaintextTailroom] fits. It reports
// whether the copy occurred. Packet descriptors must be in offset ascending order.
func copyPacketBufIfFits(dst, src *packetBuf, packets []tun.ReadPacket) bool {
	if dst == nil || len(packets) < 1 {
		return false
	}
	// validate tail descriptor as it relates to src
	tailPkt := packets[len(packets)-1]
	if tailPkt.Offset < 0 || tailPkt.Size < 1 {
		return false
	}
	end := tailPkt.Offset + tailPkt.Size
	if end < tailPkt.Offset || end > len(src.slab) ||
		outboundPlaintextTailroom > len(src.slab)-end {
		return false
	}
	// validate src fits in dst
	if end+outboundPlaintextTailroom > len(dst.slab) {
		return false
	}
	copy(dst.slab, src.slab[:end])
	return true
}

func (device *Device) RoutineReadFromTUN(id int, queue tun.Reader) {
	defer func() {
		device.log.Verbosef("Routine: TUN reader %d - stopped", id)
		device.state.stopping.Done()
		device.queue.encryption.wg.Done()
	}()

	device.log.Verbosef("Routine: TUN reader %d - started", id)

	var (
		batchSize   = device.BatchSize()
		packets     = make([]tun.ReadPacket, batchSize)
		fullSizeBuf = device.getPacketBuf()
		smallBuf    *packetBuf
		readErr     error
		elems       = make([]*QueueOutboundElement, batchSize)
		elemsByPeer = make(map[*Peer]*QueueOutboundElementsContainer, batchSize)
		count       = 0
	)

	if device.hasDistinctSmallPacketBufPool() {
		smallBuf = device.getSmallPacketBuf()
	}

	defer func() {
		fullSizeBuf.decRef()
		if smallBuf != nil {
			smallBuf.decRef()
		}
	}()

	for i := range elems {
		elems[i] = device.GetOutboundElement()
	}

	defer func() {
		for _, elem := range elems {
			if elem != nil {
				device.PutOutboundElement(elem)
			}
		}
	}()

	var (
		buf          *packetBuf // either fullSizeBuf or smallBuf
		bufHandedOff bool
	)

	for {
		if bufHandedOff {
			if buf == smallBuf {
				smallBuf.decRef()
				smallBuf = device.getSmallPacketBuf()
			} else {
				fullSizeBuf.decRef()
				fullSizeBuf = device.getPacketBuf()
			}
		}
		buf = fullSizeBuf
		bufHandedOff = false

		// read packets
		count, readErr = queue.Read(fullSizeBuf.slab, packets)
		if copyPacketBufIfFits(smallBuf, fullSizeBuf, packets[:count]) {
			buf = smallBuf
		}

		for i, meta := range packets[:count] {
			if meta.Size < 1 || meta.Size > MaxContentSize {
				continue
			}

			// explicitly pin slice capacity, so downstream expansion of length
			// never leak into an adjacent packet
			packet := buf.slab[meta.Offset : meta.Offset+meta.Size : meta.Offset+meta.Size+outboundPlaintextTailroom]

			// lookup peer
			var peer *Peer
			switch packet[0] >> 4 {
			case 4:
				if len(packet) < ipv4.HeaderLen {
					continue
				}
				src := netip.AddrFrom4([4]byte(packet[IPv4offsetSrc : IPv4offsetSrc+net.IPv4len]))
				dst := netip.AddrFrom4([4]byte(packet[IPv4offsetDst : IPv4offsetDst+net.IPv4len]))
				peer = device.allowedips.LookupFromPacket(src, dst, packet)

			case 6:
				if len(packet) < ipv6.HeaderLen {
					continue
				}
				src := netip.AddrFrom16([16]byte(packet[IPv6offsetSrc : IPv6offsetSrc+net.IPv6len]))
				dst := netip.AddrFrom16([16]byte(packet[IPv6offsetDst : IPv6offsetDst+net.IPv6len]))
				peer = device.allowedips.LookupFromPacket(src, dst, packet)

			default:
				device.log.Verbosef("Received packet with unknown IP version")
			}

			if peer == nil {
				continue
			}

			elem := elems[i]
			buf.incRef()
			bufHandedOff = true
			elem.buffer = buf
			elem.packet = packet
			elem.plaintextOffset = meta.Offset

			elemsForPeer, ok := elemsByPeer[peer]
			if !ok {
				elemsForPeer = device.GetOutboundElementsContainer()
				elemsByPeer[peer] = elemsForPeer
			}
			elemsForPeer.elems = append(elemsForPeer.elems, elem)
			elems[i] = device.GetOutboundElement()
		}

		for peer, elemsForPeer := range elemsByPeer {
			peer.StagePackets(elemsForPeer)
			peer.SendStagedPackets()
			delete(elemsByPeer, peer)
		}

		if readErr != nil {
			if errors.Is(readErr, tun.ErrTooManySegments) {
				// TODO: record stat for this
				// This will happen if MSS is surprisingly small (< 576)
				// coincident with reasonably high throughput.
				device.log.Verbosef("Dropped some packets from multi-segment read: %v", readErr)
				continue
			}
			if !device.isClosed() {
				if !errors.Is(readErr, os.ErrClosed) {
					device.log.Errorf("Failed to read packet from TUN device: %v", readErr)
				}
				go device.Close()
			}
			return
		}
	}
}

func (peer *Peer) StagePackets(elems *QueueOutboundElementsContainer) {
	if running := peer.doIfRunning(func() {
		for {
			select {
			case peer.queue.staged <- elems:
				return
			default:
			}
			select {
			case tooOld := <-peer.queue.staged:
				for _, elem := range tooOld.elems {
					peer.device.PutOutboundElement(elem)
				}
				peer.device.PutOutboundElementsContainer(tooOld)
			default:
			}
		}
	}); !running {
		for _, elem := range elems.elems {
			peer.device.PutOutboundElement(elem)
		}
		peer.device.PutOutboundElementsContainer(elems)
	}
}

// SendStagedPackets sends any staged packets to Peer.
func (peer *Peer) SendStagedPackets() {
top:
	if len(peer.queue.staged) == 0 || !peer.device.isUp() {
		return
	}

	keypair := peer.keypairs.Current()
	if keypair == nil || keypair.sendNonce.Load() >= RejectAfterMessages || time.Since(keypair.created) >= RejectAfterTime {
		peer.SendHandshakeInitiation(false)
		return
	}

	for {
		var elemsContainerOOO *QueueOutboundElementsContainer
		select {
		case elemsContainer := <-peer.queue.staged:
			i := 0
			for _, elem := range elemsContainer.elems {
				elem.peer = peer
				elem.nonce = keypair.sendNonce.Add(1) - 1
				if elem.nonce >= RejectAfterMessages {
					keypair.sendNonce.Store(RejectAfterMessages)
					if elemsContainerOOO == nil {
						elemsContainerOOO = peer.device.GetOutboundElementsContainer()
					}
					elemsContainerOOO.elems = append(elemsContainerOOO.elems, elem)
					continue
				} else {
					elemsContainer.elems[i] = elem
					i++
				}

				elem.keypair = keypair
			}
			elemsContainer.elems = elemsContainer.elems[:i]

			if elemsContainerOOO != nil {
				peer.StagePackets(elemsContainerOOO) // XXX: Out of order, but we can't front-load go chans
			}

			if len(elemsContainer.elems) == 0 {
				peer.device.PutOutboundElementsContainer(elemsContainer)
				goto top
			}

			peer.queueOutboundIfRunning(elemsContainer)

			if elemsContainerOOO != nil {
				goto top
			}
		default:
			return
		}
	}
}

func (peer *Peer) FlushStagedPackets() {
	for {
		select {
		case elemsContainer := <-peer.queue.staged:
			for _, elem := range elemsContainer.elems {
				peer.device.PutOutboundElement(elem)
			}
			peer.device.PutOutboundElementsContainer(elemsContainer)
		default:
			return
		}
	}
}

func calculatePaddingSize(packetSize, mtu int) int {
	lastUnit := packetSize
	if mtu == 0 {
		return ((lastUnit + PaddingMultiple - 1) & ^(PaddingMultiple - 1)) - lastUnit
	}
	if lastUnit > mtu {
		lastUnit %= mtu
	}
	paddedSize := ((lastUnit + PaddingMultiple - 1) & ^(PaddingMultiple - 1))
	if paddedSize > mtu {
		paddedSize = mtu
	}
	return paddedSize - lastUnit
}

/* Encrypts the elements in the queue
 * and marks them for sequential consumption (by releasing the mutex)
 *
 * Obs. One instance per core
 */
func (device *Device) RoutineEncryption(id int) {
	var paddingZeros [PaddingMultiple]byte
	var nonce [chacha20poly1305.NonceSize]byte

	defer device.log.Verbosef("Routine: encryption worker %d - stopped", id)
	device.log.Verbosef("Routine: encryption worker %d - started", id)

	for elemsContainer := range device.queue.encryption.c {
		for _, elem := range elemsContainer.elems {
			// populate header fields
			header := elem.buffer.slab[elem.plaintextOffset-MessageTransportHeaderSize : elem.plaintextOffset]

			fieldType := header[0:4]
			fieldReceiver := header[4:8]
			fieldNonce := header[8:16]

			binary.LittleEndian.PutUint32(fieldType, MessageTransportType)
			binary.LittleEndian.PutUint32(fieldReceiver, elem.keypair.remoteIndex)
			binary.LittleEndian.PutUint64(fieldNonce, elem.nonce)

			// pad content to multiple of 16
			paddingSize := calculatePaddingSize(len(elem.packet), int(device.tun.mtu.Load()))

			elem.packet = append(elem.packet, paddingZeros[:paddingSize]...)

			// encrypt content and release to consumer

			binary.LittleEndian.PutUint64(nonce[4:], elem.nonce)
			elem.packet = elem.keypair.send.Seal(
				header,
				nonce[:],
				elem.packet,
				nil,
			)

			// re-slice packet to include encapsulating transport space
			start := elem.plaintextOffset - MessageTransportHeaderSize - MessageEncapsulatingTransportSize
			end := elem.plaintextOffset - MessageTransportHeaderSize + len(elem.packet)
			elem.packet = elem.buffer.slab[start:end]
		}
		elemsContainer.filling.Done()
	}
}

func (peer *Peer) RoutineSequentialSender(maxBatchSize int) {
	device := peer.device
	defer func() {
		defer device.log.Verbosef("%v - Routine: sequential sender - stopped", peer)
		peer.runningState.queueReaders.Done()
	}()
	device.log.Verbosef("%v - Routine: sequential sender - started", peer)

	bufs := make([][]byte, 0, maxBatchSize)

	for elemsContainer := range peer.queue.outbound {
		peer.processOutboundContainer(elemsContainer, bufs[:0])
	}
}

// processOutboundContainer waits for the encryption routine to finish
// filling elemsContainer, then sends the batch (or drops it, if the peer
// has been stopped) and returns the container to the pool.
//
// scratch is a length-0 slice used to assemble the per-packet buffers
// passed to SendBuffers; its backing array is reused across calls.
func (peer *Peer) processOutboundContainer(elemsContainer *QueueOutboundElementsContainer, scratch [][]byte) {
	// Invariants from RoutineSequentialSender; all should be unreachable.
	if len(scratch) != 0 || cap(scratch) == 0 {
		panic(fmt.Sprintf("processOutboundContainer: scratch must be empty with non-zero cap; got len=%d cap=%d",
			len(scratch), cap(scratch)))
	}
	if cap(scratch) < len(elemsContainer.elems) {
		panic(fmt.Sprintf("processOutboundContainer: scratch cap %d < elems %d",
			cap(scratch), len(elemsContainer.elems)))
	}

	device := peer.device
	defer device.PutOutboundElementsContainer(elemsContainer)

	// Wait for RoutineEncryption to finish filling the container. After
	// Wait returns we have happens-before with that goroutine and are the
	// sole owner of the container until Put hands it back to the pool.
	elemsContainer.filling.Wait()

	if !peer.runningState.isRunning.Load() {
		// peer has been stopped; return re-usable elems to the shared pool.
		// This is an optimization only. It is possible for the peer to be stopped
		// immediately after this check, in which case, elem will get processed.
		// The timers and SendBuffers code are resilient to a few stragglers.
		// TODO: rework peer shutdown order to ensure
		// that we never accidentally keep timers alive longer than necessary.
		for _, elem := range elemsContainer.elems {
			device.PutOutboundElement(elem)
		}
		return
	}

	dataSent := false
	for _, elem := range elemsContainer.elems {
		if len(elem.packet[MessageEncapsulatingTransportSize:]) != MessageKeepaliveSize {
			dataSent = true
		}
		scratch = append(scratch, elem.packet)
	}

	peer.timersAnyAuthenticatedPacketTraversal()
	peer.timersAnyAuthenticatedPacketSent()

	err := peer.SendBuffers(scratch)
	if dataSent {
		peer.timersDataSent()
	}
	for _, elem := range elemsContainer.elems {
		device.PutOutboundElement(elem)
	}
	if err != nil {
		var errGSO conn.ErrUDPGSODisabled
		if errors.As(err, &errGSO) {
			device.log.Verbosef(err.Error())
			err = errGSO.RetryErr
		}
	}
	if err != nil {
		device.log.Errorf("%v - Failed to send data packets: %v", peer, err)
		return
	}

	peer.keepKeyFreshSending()
}
