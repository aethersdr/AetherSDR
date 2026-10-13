/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2017-2023 WireGuard LLC. All Rights Reserved.
 */

package device

import (
	"container/list"
	"encoding/binary"
	"errors"
	"net/netip"
	"runtime"
	"slices"
	"sync"
	"sync/atomic"
	"time"

	"github.com/tailscale/wireguard-go/conn"
)

type Peer struct {
	runningState struct {
		// isRunning is swapped across [Peer.Start] and [Peer.Stop]. Direct
		// access loads are best-effort and inherently racey.
		isRunning atomic.Bool
		// queueWriters is the count of outstanding goroutines performing
		// potential write operations against [Peer.queue].* channels.
		// It is semantically similar to a [sync.WaitGroup], but free
		// from the event order constraints of [sync.WaitGroup.Add]. Increments
		// from zero may run concurrent to the "waiting" side.
		queueWriters atomic.Int32
		// queueReaders is the count of outstanding goroutines performing
		// potential read operations against [Peer.queue.inbound] and
		// [Peer.queue.outbound].
		queueReaders sync.WaitGroup
	}

	keypairs            Keypairs
	handshake           Handshake
	device              *Device
	txBytes             atomic.Uint64 // bytes send to peer (endpoint)
	rxBytes             atomic.Uint64 // bytes received from peer
	lastHandshakeNano   atomic.Int64  // nanoseconds since epoch
	handshakeOnUserSend atomic.Bool   // attempt handshake following next outbound transport

	sessionState struct {
		sync.Mutex
		current        PeerSessionState
		sessionExpires time.Time
	}

	// deleteOnIdle indicates whether the peer should be deleted when idle
	// because it was auto-created via a Device.PeerLookupFunc.
	//
	// This field should only be set once, before the peer is started.
	deleteOnIdle bool

	endpoint struct {
		sync.Mutex
		val          conn.Endpoint
		clearSrcOnTx bool // signal to val.ClearSrc() prior to next packet transmission
	}

	timers struct {
		retransmitHandshake     *Timer
		sendKeepalive           *Timer
		newHandshake            *Timer
		sessionExpired          *Timer
		zeroKeyMaterial         *Timer
		persistentKeepalive     *Timer
		handshakeAttempts       atomic.Uint32
		needAnotherKeepalive    atomic.Bool
		sentLastMinuteHandshake atomic.Bool
	}

	state struct {
		sync.Mutex // protects against concurrent Start/Stop, and fields below

		allowedIPs []netip.Prefix

		// testAllowedIP, if non-nil, is used to test whether the peer is
		// allowed to send a packet from the given IP address. It can be read
		// without locking, but must be set with the state mutex locked.
		testAllowedIP atomic.Pointer[func(netip.Addr) bool]
	}

	// queue channel write operations must be wrapped by [Peer.doIfRunning]
	queue struct {
		staged   chan *QueueOutboundElementsContainer // staged packets before a handshake is available
		outbound chan *QueueOutboundElementsContainer // sequential ordering of udp transmission
		inbound  chan *QueueInboundElementsContainer  // sequential ordering of tun writing
	}

	cookieGenerator             CookieGenerator
	trieEntries                 list.List
	persistentKeepaliveInterval atomic.Uint32

	// flowID pins this peer's decrypted packets to a single TUN write queue
	// when the device is a [tun.MultiQueueDevice].
	// See [Peer.writeTUN].
	//
	// The kernel picks a TUN queue by 5-tuple for new (or quiescent,
	// TUN_FLOW_EXPIRE=3s) flows. A pin learned from our writes overrides
	// the hash. Kernel delivers a packet to the stack before recording the pin,
	// so the first reply to a flow we write leaves on the hashed queue and the
	// rest on ours. Both queues then carry that flow briefly, which may
	// introduce overlay reordering.
	// See https://elixir.bootlin.com/linux/v7.0/source/drivers/net/tun.c#L461
	//
	// TODO match or influence kernel hashing when writing to TUN to avoid
	// reorders. Note that the kernel hash is siphash keyed by an unexported
	// random value. This can be overridden with TUNSETSTEERINGEBPF ioctl.
	flowID uint32
}

func (device *Device) NewPeer(pk NoisePublicKey) (*Peer, error) {
	if device.isClosed() {
		return nil, errors.New("device closed")
	}

	// lock resources
	device.staticIdentity.RLock()
	defer device.staticIdentity.RUnlock()

	device.peers.Lock()
	defer device.peers.Unlock()

	// check if over limit
	if len(device.peers.keyMap) >= MaxPeers {
		return nil, errors.New("too many peers")
	}

	// create peer
	peer := new(Peer)

	peer.cookieGenerator.Init(pk)
	peer.device = device
	// Public keys are uniformly distributed, so the low bytes spread fine.
	peer.flowID = binary.LittleEndian.Uint32(pk[:4])

	// staged is never closed, and it can be accessed concurrent to [Peer.Start],
	// so we init here instead of [Peer.Start].
	peer.queue.staged = make(chan *QueueOutboundElementsContainer, device.config.queueStagedSize)

	// map public key
	_, ok := device.peers.keyMap[pk]
	if ok {
		return nil, errAddExistingPeer
	}

	// pre-compute DH
	handshake := &peer.handshake
	handshake.mutex.Lock()
	handshake.precomputedStaticStatic, _ = device.staticIdentity.privateKey.sharedSecret(pk)
	handshake.remoteStatic = pk
	handshake.mutex.Unlock()

	// reset endpoint
	peer.endpoint.Lock()
	peer.endpoint.val = nil
	peer.endpoint.clearSrcOnTx = false
	peer.endpoint.Unlock()

	// init timers
	peer.timersInit()

	// add
	device.peers.keyMap[pk] = peer

	return peer, nil
}

// SetAllowedIPs sets the allowed IP prefixes for this peer.
//
// If the allowedIPs are unchanged since the last call, this method is a no-op.
// It's the caller's responsibility to ensure that no two peers have duplicate
// allowed IPs. If so, the last writer wins.
func (p *Peer) SetAllowedIPs(allowedIPs []netip.Prefix) {
	p.state.Lock()
	defer p.state.Unlock()

	if slices.Equal(p.state.allowedIPs, allowedIPs) {
		return
	}
	p.device.allowedips.setPeerPrefixes(p, allowedIPs)

	allowedIPs = slices.Clone(allowedIPs) // avoid retaining caller's slice
	p.state.allowedIPs = allowedIPs

	f := mkIPInCIDRsTestFunc(allowedIPs)
	p.state.testAllowedIP.Store(&f)
}

// SetPresharedKey sets the optional WireGuard pre-shared key for this peer.
// The zero value disables the pre-shared-key layer.
func (p *Peer) SetPresharedKey(psk NoisePresharedKey) {
	p.handshake.mutex.Lock()
	defer p.handshake.mutex.Unlock()
	p.handshake.presharedKey = psk
}

// SendBuffers sends buffers to peer. WireGuard packet data in each element of
// buffers must be preceded by MessageEncapsulatingTransportSize number of
// bytes.
func (peer *Peer) SendBuffers(buffers [][]byte) error {
	peer.device.net.RLock()
	defer peer.device.net.RUnlock()

	if peer.device.isClosed() {
		return nil
	}

	peer.endpoint.Lock()
	endpoint := peer.endpoint.val
	if endpoint == nil {
		peer.endpoint.Unlock()
		return errors.New("no known endpoint for peer")
	}
	if peer.endpoint.clearSrcOnTx {
		endpoint.ClearSrc()
		peer.endpoint.clearSrcOnTx = false
	}
	peer.endpoint.Unlock()

	err := peer.device.net.bind.Send(buffers, endpoint, MessageEncapsulatingTransportSize)
	if err == nil {
		var totalLen uint64
		for _, b := range buffers {
			totalLen += uint64(len(b))
		}
		peer.txBytes.Add(totalLen)
	}
	return err
}

// writeTUN writes bufs to the TUN queue this peer is pinned to, keeping the
// peer's decrypted packets in order.
func (peer *Peer) writeTUN(bufs [][]byte, offset int) (int, error) {
	return peer.device.tun.writeTo(int(peer.flowID), bufs, offset)
}

func (peer *Peer) String() string {
	// The awful goo that follows is identical to:
	//
	//   base64Key := base64.StdEncoding.EncodeToString(peer.handshake.remoteStatic[:])
	//   abbreviatedKey := base64Key[0:4] + "…" + base64Key[39:43]
	//   return fmt.Sprintf("peer(%s)", abbreviatedKey)
	//
	// except that it is considerably more efficient.
	src := peer.handshake.remoteStatic
	b64 := func(input byte) byte {
		return input + 'A' + byte(((25-int(input))>>8)&6) - byte(((51-int(input))>>8)&75) - byte(((61-int(input))>>8)&15) + byte(((62-int(input))>>8)&3)
	}
	b := []byte("peer(____…____)")
	const first = len("peer(")
	const second = len("peer(____…")
	b[first+0] = b64((src[0] >> 2) & 63)
	b[first+1] = b64(((src[0] << 4) | (src[1] >> 4)) & 63)
	b[first+2] = b64(((src[1] << 2) | (src[2] >> 6)) & 63)
	b[first+3] = b64(src[2] & 63)
	b[second+0] = b64(src[29] & 63)
	b[second+1] = b64((src[30] >> 2) & 63)
	b[second+2] = b64(((src[30] << 4) | (src[31] >> 4)) & 63)
	b[second+3] = b64((src[31] << 2) & 63)
	return string(b)
}

func (peer *Peer) Start() {
	// should never start a peer on a closed device
	if peer.device.isClosed() {
		return
	}

	// prevent simultaneous start/stop operations
	peer.state.Lock()
	defer peer.state.Unlock()

	if peer.runningState.isRunning.Load() {
		return
	}

	device := peer.device
	device.log.Verbosef("%v - Starting", peer)

	// init inbound & outbound packet queues
	peer.queue.outbound = make(chan *QueueOutboundElementsContainer, device.config.queueOutboundSize)
	peer.queue.inbound = make(chan *QueueInboundElementsContainer, device.config.queueInboundSize)

	// reset routine state
	peer.runningState.queueReaders.Wait()
	peer.runningState.queueReaders.Add(2)

	peer.handshake.mutex.Lock()
	peer.handshake.lastSentHandshake = time.Now().Add(-(RekeyTimeout + time.Second))
	peer.handshake.mutex.Unlock()

	peer.device.queue.encryption.wg.Add(1) // keep encryption queue open for our writes

	peer.timersStart()

	// Use the device batch size, not the bind batch size, as the device size is
	// the size of the batch pools.
	batchSize := peer.device.BatchSize()
	go peer.RoutineSequentialSender(batchSize)
	go peer.RoutineSequentialReceiver(batchSize)

	peer.runningState.isRunning.Store(true)

	// A lazily-created peer that never completes a handshake otherwise never
	// arms its reaping timer. Arm it here, while running under state.Lock, so
	// it's reclaimed after RejectAfterTime*3 of no session and is guaranteed to
	// be torn down by a matching Stop. A completed handshake re-Mods it.
	if peer.deleteOnIdle {
		peer.timers.zeroKeyMaterial.Mod(RejectAfterTime * 3)
	}
}

func (peer *Peer) ZeroAndFlushAll() {
	device := peer.device
	if peer.timers.sessionExpired != nil {
		peer.timers.sessionExpired.Del()
	}

	// clear key pairs

	keypairs := &peer.keypairs
	keypairs.Lock()
	device.DeleteKeypair(keypairs.previous)
	device.DeleteKeypair(keypairs.current)
	device.DeleteKeypair(keypairs.next.Load())
	keypairs.previous = nil
	keypairs.current = nil
	keypairs.next.Store(nil)
	keypairs.Unlock()

	// clear handshake state

	handshake := &peer.handshake
	handshake.mutex.Lock()
	device.indexTable.Delete(handshake.localIndex)
	handshake.Clear()
	handshake.mutex.Unlock()

	peer.FlushStagedPackets()

	peer.sessionState.Lock()
	peer.sessionState.sessionExpires = time.Time{}
	peer.noteSessionStateLocked(PeerSessionNone)
	peer.sessionState.Unlock()
}

func (peer *Peer) ExpireCurrentKeypairs() {
	handshake := &peer.handshake
	handshake.mutex.Lock()
	peer.device.indexTable.Delete(handshake.localIndex)
	handshake.Clear()
	peer.handshake.lastSentHandshake = time.Now().Add(-(RekeyTimeout + time.Second))
	handshake.mutex.Unlock()

	keypairs := &peer.keypairs
	keypairs.Lock()
	if keypairs.current != nil {
		keypairs.current.sendNonce.Store(RejectAfterMessages)
	}
	if next := keypairs.next.Load(); next != nil {
		next.sendNonce.Store(RejectAfterMessages)
	}
	keypairs.Unlock()

	peer.sessionState.Lock()
	peer.sessionState.sessionExpires = time.Time{}
	peer.noteSessionStateLocked(PeerSessionExpired)
	peer.sessionState.Unlock()
}

// queueOutboundIfRunning queues elems for outbound encryption & transmission to
// peer if peer is running, otherwise elems are returned to their respective
// pools for later re-use.
func (peer *Peer) queueOutboundIfRunning(elems *QueueOutboundElementsContainer) (queued bool) {
	if queued = peer.doIfRunning(func() {
		elems.filling.Add(1)
		peer.queue.outbound <- elems
		peer.device.queue.encryption.c <- elems
	}); !queued {
		for _, elem := range elems.elems {
			peer.device.PutOutboundElement(elem)
		}
		peer.device.PutOutboundElementsContainer(elems)
	}
	return queued
}

// queueInboundIfRunning queues elems for inbound decryption from peer if peer
// is running, otherwise elems are returned to their respective pools for later
// re-use.
func (peer *Peer) queueInboundIfRunning(elems *QueueInboundElementsContainer) (queued bool) {
	if queued = peer.doIfRunning(func() {
		elems.filling.Add(1)
		peer.queue.inbound <- elems
		peer.device.queue.decryption.c <- elems
	}); !queued {
		for _, elem := range elems.elems {
			peer.device.PutInboundElement(elem)
		}
		peer.device.PutInboundElementsContainer(elems)
	}
	return queued
}

// doIfRunning executes doQueueCrit if the [Peer] is running, i.e. the [Peer] is
// not stopped or stopping. It returns true if doQueueCrit ran, otherwise it
// returns false.
//
// doIfRunning exists to enable safe, bare (no select{}) write operations against
// [Peer.queue].* channels.
//
// doIfRunning makes no guarantee about the running generation of the [Peer], i.e.
// a [Peer] may stop then start all while a goroutine is within doIfRunning, but
// before doQueueCrit has run.
//
// doIfRunning is built on the assumption that the supplied critical sections are
// relatively cheap, as the mutually excluded peer-stopping side
// ([Peer.waitQueueActors]) busy-waits for actors to exit.
func (peer *Peer) doIfRunning(doQueueCrit func()) (ranCrit bool) {
	if !peer.runningState.isRunning.Load() {
		return false
	}
	peer.runningState.queueWriters.Add(1)
	defer peer.runningState.queueWriters.Add(-1)
	if peer.runningState.isRunning.Load() {
		doQueueCrit()
		return true
	}
	return false
}

// waitQueueActors waits for [Peer.runningState.queueWriters] and [Peer.runningState.queueReaders]
// to reach zero. It should be called after [Peer.runningState.isRunning] is
// swapped false.
func (peer *Peer) waitQueueActors() {
	drainInboundOutbound := func() {
		for {
			select {
			case container := <-peer.queue.inbound:
				container.filling.Wait()
				for _, elem := range container.elems {
					peer.device.PutInboundElement(elem)
				}
				peer.device.PutInboundElementsContainer(container)
			case container := <-peer.queue.outbound:
				container.filling.Wait()
				for _, elem := range container.elems {
					peer.device.PutOutboundElement(elem)
				}
				peer.device.PutOutboundElementsContainer(container)
			default:
				return
			}
		}
	}

	// Busy-wait for queue writers to return.
	for peer.runningState.queueWriters.Load() != 0 {
		// We drain at every iteration to give our busy-wait some meaningful
		// work that can unblock actors performing channel writes. The goal at
		// this point is to get them to return as soon as possible. It's ok if
		// there are outstanding elements on packet queues once queueWriters
		// reaches zero, our reader shutdown below will handle those stragglers.
		drainInboundOutbound()
		// We don't want our busy-wait starving actors, so explicitly yield back
		// to the runtime, to bias towards actor goroutines getting CPU time.
		runtime.Gosched()
	}

	// Signal to RoutineSequentialSender and RoutineSequentialReceiver to
	// exit. Let them deal with any outstanding elements on the associated
	// channels.
	close(peer.queue.inbound)
	close(peer.queue.outbound)
	peer.runningState.queueReaders.Wait()
}

func (peer *Peer) Stop() {
	peer.state.Lock()
	defer peer.state.Unlock()

	if !peer.runningState.isRunning.Swap(false) {
		return
	}

	peer.waitQueueActors()

	peer.device.log.Verbosef("%v - Stopping", peer)

	peer.timersStop()
	peer.device.queue.encryption.wg.Done() // no more writes to encryption queue from us

	peer.ZeroAndFlushAll()
}

func (peer *Peer) noteSessionState(state PeerSessionState) {
	peer.sessionState.Lock()
	defer peer.sessionState.Unlock()
	peer.noteSessionStateLocked(state)
}

// noteSessionStateLocked records a session state transition and delivers the
// callback. The caller must hold peer.sessionState.Mutex during the
// state determination and transition.
func (peer *Peer) noteSessionStateLocked(state PeerSessionState) {
	if peer.sessionState.current == state {
		return
	}
	peer.sessionState.current = state
	if f := peer.device.peerStateFn.Load(); f != nil {
		(*f)(peer.handshake.remoteStatic, state)
	}
}

func (peer *Peer) noteSessionHandshakeStarted() {
	peer.sessionState.Lock()
	defer peer.sessionState.Unlock()
	if peer.sessionState.current == PeerSessionEstablished {
		return
	}
	peer.noteSessionStateLocked(PeerSessionHandshake)
}

func (peer *Peer) noteSessionHandshakeStopped() {
	peer.sessionState.Lock()
	defer peer.sessionState.Unlock()
	state := PeerSessionNone
	if peer.hasKeyMaterial() {
		state = PeerSessionExpired
	}
	peer.noteSessionStateLocked(state)
}

func (peer *Peer) hasKeyMaterial() bool {
	keypairs := &peer.keypairs
	keypairs.RLock()
	defer keypairs.RUnlock()
	return keypairs.previous != nil || keypairs.current != nil || keypairs.next.Load() != nil
}

func (peer *Peer) markEndpointSrcForClearing() {
	peer.endpoint.Lock()
	defer peer.endpoint.Unlock()
	if peer.endpoint.val == nil {
		return
	}
	peer.endpoint.clearSrcOnTx = true
}
