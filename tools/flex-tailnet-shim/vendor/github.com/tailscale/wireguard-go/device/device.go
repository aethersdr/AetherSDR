/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2017-2023 WireGuard LLC. All Rights Reserved.
 */

package device

import (
	"errors"
	"net/netip"
	"runtime"
	"sync"
	"sync/atomic"
	"time"

	"github.com/tailscale/wireguard-go/conn"
	"github.com/tailscale/wireguard-go/ratelimiter"
	"github.com/tailscale/wireguard-go/rwcancel"
	"github.com/tailscale/wireguard-go/tun"
)

type Device struct {
	config config

	state struct {
		// state holds the device's state. It is accessed atomically.
		// Use the device.deviceState method to read it.
		// device.deviceState does not acquire the mutex, so it captures only a snapshot.
		// During state transitions, the state variable is updated before the device itself.
		// The state is thus either the current state of the device or
		// the intended future state of the device.
		// For example, while executing a call to Up, state will be deviceStateUp.
		// There is no guarantee that that intended future state of the device
		// will become the actual state; Up can fail.
		// The device can also change state multiple times between time of check and time of use.
		// Unsynchronized uses of state must therefore be advisory/best-effort only.
		state atomic.Uint32 // actually a deviceState, but typed uint32 for convenience
		// stopping blocks until all inputs to Device have been closed.
		stopping sync.WaitGroup
		// mu protects state changes.
		sync.Mutex
	}

	net struct {
		stopping sync.WaitGroup
		sync.RWMutex
		bind          conn.Bind // bind interface
		netlinkCancel *rwcancel.RWCancel
		port          uint16 // listening port
		fwmark        uint32 // mark value (0 = disabled)
	}

	staticIdentity struct {
		sync.RWMutex
		privateKey NoisePrivateKey
		publicKey  NoisePublicKey
	}

	peers struct {
		sync.RWMutex // protects keyMap
		keyMap       map[NoisePublicKey]*Peer
		lookupFunc   PeerLookupFunc // or nil if unused
	}

	peerStateFn   atomic.Pointer[PeerSessionStateFunc]    // observes peer session state changes, nil if unset
	priorityMsgFn atomic.Pointer[PeerPriorityMessageFunc] // returns a priority message to be sent around session establishment, nil if unset

	rate struct {
		underLoadUntil atomic.Int64
		limiter        ratelimiter.Ratelimiter
	}

	allowedips    AllowedIPs
	indexTable    IndexTable
	cookieChecker CookieChecker

	pool struct {
		inboundElementsContainer  *WaitPool
		outboundElementsContainer *WaitPool
		inboundElements           *WaitPool
		outboundElements          *WaitPool
		packetBufs                *WaitPool
		// smallPacketBufs aliases packetBufs when no distinct small packet pool
		// is used. See [Device.PopulatePools].
		smallPacketBufs *WaitPool
	}

	queue struct {
		encryption *outboundQueue
		decryption *inboundQueue
		handshake  *handshakeQueue
	}

	tun struct {
		// Fields in this block are read-only after instantiation.
		device  tun.Device
		queues  []tun.Queue // device's read queues, at least one, see [tun.QueuesOf]
		writeTo func(flow int, bufs [][]byte, offset int) (int, error)

		mtu atomic.Int32
	}

	ipcMutex sync.RWMutex
	closed   chan struct{}
	log      *Logger
}

type config struct {
	queueStagedSize            int
	queueOutboundSize          int
	queueInboundSize           int
	queueHandshakeSize         int
	preallocatedBuffersPerPool uint32
	metrics                    Metrics
}

func defaultConfig() config {
	return config{
		queueStagedSize:            DefaultQueueStagedSize,
		queueOutboundSize:          DefaultQueueOutboundSize,
		queueInboundSize:           DefaultQueueInboundSize,
		queueHandshakeSize:         DefaultQueueHandshakeSize,
		preallocatedBuffersPerPool: DefaultPreallocatedBuffersPerPool,
	}
}

// An Option configures a [Device].
type Option interface {
	apply(*config)
}

type optionFunc func(*config)

func (f optionFunc) apply(config *config) {
	f(config)
}

// WithQueueStagedSize sets the capacity of each peer's staged packet queue.
// Staged packet queues must be buffered, so max(tun readers, size) is applied
// to the user-supplied value. [DefaultQueueStagedSize] is the default.
func WithQueueStagedSize(size int) Option {
	return optionFunc(func(config *config) {
		config.queueStagedSize = max(1, size)
	})
}

// WithQueueOutboundSize sets the capacity of each peer's outbound packet queue.
// It also sets the capacity of the device-wide outbound queue, whose total
// capacity is size multiplied by the number of TUN queues.
// [DefaultQueueOutboundSize] is the default.
func WithQueueOutboundSize(size int) Option {
	return optionFunc(func(config *config) {
		config.queueOutboundSize = size
	})
}

// WithQueueInboundSize sets the capacity of the device and per-peer inbound
// packet queues. [DefaultQueueInboundSize] is the default.
func WithQueueInboundSize(size int) Option {
	return optionFunc(func(config *config) {
		config.queueInboundSize = size
	})
}

// WithQueueHandshakeSize sets the capacity of the device's handshake queue.
// [DefaultQueueHandshakeSize] is the default.
func WithQueueHandshakeSize(size int) Option {
	return optionFunc(func(config *config) {
		config.queueHandshakeSize = size
	})
}

// WithPreallocatedBuffersPerPool sets the maximum number of packet memory
// pool outstanding items. A value of zero is unlimited. Care must be taken to
// not set a size that is too small, which can lead to immediate deadlock, or
// increase the probability of deadlock once packets start flowing.
// See tailscale/corp#46396. [DefaultPreallocatedBuffersPerPool] is the default.
func WithPreallocatedBuffersPerPool(size uint32) Option {
	return optionFunc(func(config *config) {
		config.preallocatedBuffersPerPool = size
	})
}

// Counter is a monotonically increasing counter used in [Metrics].
type Counter interface {
	// Add increments the Counter's value by n. n must not be negative. Add is
	// called in performance-sensitive contexts, therefore it must be cheap. Add
	// may be called concurrently.
	Add(n int64)
}

// Metrics contains metrics that can be exported via [WithMetrics].
type Metrics struct {
	// MessageInitiationTXAttemptInitial counts non-retry handshake
	// initiation messages passed to the network send path.
	MessageInitiationTXAttemptInitial Counter
	// MessageInitiationTXAttemptRetry counts retry handshake initiation
	// messages passed to the network send path.
	MessageInitiationTXAttemptRetry Counter
	// MessageResponseTXAttempt counts handshake response messages passed to the
	// network send path.
	MessageResponseTXAttempt Counter
	// MessageCookieReplyTXAttempt counts cookie reply messages passed to the
	// network send path.
	MessageCookieReplyTXAttempt Counter
	// HandshakeInitiatorCompleted counts symmetric session establishment on the
	// initiator side.
	HandshakeInitiatorCompleted Counter
	// HandshakeResponderCompleted counts responder-side handshake completions,
	// observed when an authenticated transport packet confirms the new keypair.
	HandshakeResponderCompleted Counter
	// MessageTransportRXDroppedReplay counts inbound transport packets rejected by the
	// replay filter.
	MessageTransportRXDroppedReplay Counter
}

// noopCounter is a noop implementation of [Counter].
type noopCounter struct {
}

func (noopCounter) Add(int64) {}

func (m *Metrics) fillNils() {
	if m.MessageInitiationTXAttemptInitial == nil {
		m.MessageInitiationTXAttemptInitial = noopCounter{}
	}
	if m.MessageInitiationTXAttemptRetry == nil {
		m.MessageInitiationTXAttemptRetry = noopCounter{}
	}
	if m.MessageResponseTXAttempt == nil {
		m.MessageResponseTXAttempt = noopCounter{}
	}
	if m.MessageCookieReplyTXAttempt == nil {
		m.MessageCookieReplyTXAttempt = noopCounter{}
	}
	if m.HandshakeInitiatorCompleted == nil {
		m.HandshakeInitiatorCompleted = noopCounter{}
	}
	if m.HandshakeResponderCompleted == nil {
		m.HandshakeResponderCompleted = noopCounter{}
	}
	if m.MessageTransportRXDroppedReplay == nil {
		m.MessageTransportRXDroppedReplay = noopCounter{}
	}
}

// WithMetrics sets the provided [Metrics] to be used at instrumentation points.
func WithMetrics(metrics Metrics) Option {
	return optionFunc(func(config *config) {
		config.metrics = metrics
	})
}

// deviceState represents the state of a Device.
// There are three states: down, up, closed.
// Transitions:
//
//	down -----+
//	  ↑↓      ↓
//	  up -> closed
type deviceState uint32

//go:generate go run golang.org/x/tools/cmd/stringer -type deviceState -trimprefix=deviceState
const (
	deviceStateDown deviceState = iota
	deviceStateUp
	deviceStateClosed
)

// deviceState returns device.state.state as a deviceState
// See those docs for how to interpret this value.
func (device *Device) deviceState() deviceState {
	return deviceState(device.state.state.Load())
}

// isClosed reports whether the device is closed (or is closing).
// See device.state.state comments for how to interpret this value.
func (device *Device) isClosed() bool {
	return device.deviceState() == deviceStateClosed
}

// isUp reports whether the device is up (or is attempting to come up).
// See device.state.state comments for how to interpret this value.
func (device *Device) isUp() bool {
	return device.deviceState() == deviceStateUp
}

// Must hold device.peers.Lock()
func removePeerLocked(device *Device, peer *Peer, key NoisePublicKey) {
	// stop routing and processing of packets
	device.allowedips.RemoveByPeer(peer)
	peer.Stop()

	// remove from peer map
	delete(device.peers.keyMap, key)
}

// changeState attempts to change the device state to match want.
func (device *Device) changeState(want deviceState) (err error) {
	device.state.Lock()
	defer device.state.Unlock()
	old := device.deviceState()
	if old == deviceStateClosed {
		// once closed, always closed
		device.log.Verbosef("Interface closed, ignored requested state %s", want)
		return nil
	}
	switch want {
	case old:
		return nil
	case deviceStateUp:
		device.state.state.Store(uint32(deviceStateUp))
		err = device.upLocked()
		if err == nil {
			break
		}
		fallthrough // up failed; bring the device all the way back down
	case deviceStateDown:
		device.state.state.Store(uint32(deviceStateDown))
		errDown := device.downLocked()
		if err == nil {
			err = errDown
		}
	}
	device.log.Verbosef("Interface state was %s, requested %s, now %s", old, want, device.deviceState())
	return
}

// upLocked attempts to bring the device up and reports whether it succeeded.
// The caller must hold device.state.mu and is responsible for updating device.state.state.
func (device *Device) upLocked() error {
	if err := device.BindUpdate(); err != nil {
		device.log.Errorf("Unable to update bind: %v", err)
		return err
	}

	// The IPC set operation waits for peers to be created before calling Start() on them,
	// so if there's a concurrent IPC set request happening, we should wait for it to complete.
	device.ipcMutex.Lock()
	defer device.ipcMutex.Unlock()

	// Collect peers under RLock and then release before calling into them,
	// because SendKeepalive can reach CreateMessageInitiation which acquires
	// staticIdentity.RLock; holding peers.RLock across that path would
	// invert the staticIdentity < peers hierarchy (see lock-ordering.md).
	device.peers.RLock()
	peers := make([]*Peer, 0, len(device.peers.keyMap))
	for _, peer := range device.peers.keyMap {
		peers = append(peers, peer)
	}
	device.peers.RUnlock()
	for _, peer := range peers {
		peer.Start()
		if peer.persistentKeepaliveInterval.Load() > 0 {
			peer.SendKeepalive()
		}
	}
	return nil
}

// downLocked attempts to bring the device down.
// The caller must hold device.state.mu and is responsible for updating device.state.state.
func (device *Device) downLocked() error {
	err := device.BindClose()
	if err != nil {
		device.log.Errorf("Bind close failed: %v", err)
	}

	device.peers.RLock()
	for _, peer := range device.peers.keyMap {
		peer.Stop()
	}
	device.peers.RUnlock()
	return err
}

func (device *Device) Up() error {
	return device.changeState(deviceStateUp)
}

func (device *Device) Down() error {
	return device.changeState(deviceStateDown)
}

func (device *Device) IsUnderLoad() bool {
	// check if currently under load
	now := time.Now()
	// max(1, ...) is required on the right hand side, otherwise underLoad would
	// always be true when queueHandshakeSize < 8.
	underLoad := len(device.queue.handshake.c) >= max(1, device.config.queueHandshakeSize/8)
	if underLoad {
		device.rate.underLoadUntil.Store(now.Add(UnderLoadAfterTime).UnixNano())
		return true
	}
	// check if recently under load
	return device.rate.underLoadUntil.Load() > now.UnixNano()
}

func (device *Device) SetPrivateKey(sk NoisePrivateKey) error {
	// lock required resources

	device.staticIdentity.Lock()
	defer device.staticIdentity.Unlock()

	if sk.Equals(device.staticIdentity.privateKey) {
		return nil
	}

	device.peers.Lock()
	defer device.peers.Unlock()

	lockedPeers := make([]*Peer, 0, len(device.peers.keyMap))
	for _, peer := range device.peers.keyMap {
		peer.handshake.mutex.RLock()
		lockedPeers = append(lockedPeers, peer)
	}

	// remove peers with matching public keys

	publicKey := sk.publicKey()
	for key, peer := range device.peers.keyMap {
		if peer.handshake.remoteStatic.Equals(publicKey) {
			peer.handshake.mutex.RUnlock()
			removePeerLocked(device, peer, key)
			peer.handshake.mutex.RLock()
		}
	}

	// update key material

	device.staticIdentity.privateKey = sk
	device.staticIdentity.publicKey = publicKey
	device.cookieChecker.Init(publicKey)

	// do static-static DH pre-computations

	expiredPeers := make([]*Peer, 0, len(device.peers.keyMap))
	for _, peer := range device.peers.keyMap {
		handshake := &peer.handshake
		handshake.precomputedStaticStatic, _ = device.staticIdentity.privateKey.sharedSecret(handshake.remoteStatic)
		expiredPeers = append(expiredPeers, peer)
	}

	for _, peer := range lockedPeers {
		peer.handshake.mutex.RUnlock()
	}
	for _, peer := range expiredPeers {
		peer.ExpireCurrentKeypairs()
	}

	return nil
}

func NewDevice(tunDevice tun.Device, bind conn.Bind, logger *Logger, opts ...Option) *Device {
	device := &Device{config: defaultConfig()}
	for _, opt := range opts {
		opt.apply(&device.config)
	}
	device.config.metrics.fillNils()
	device.state.state.Store(uint32(deviceStateDown))
	device.closed = make(chan struct{})
	device.log = logger
	device.net.bind = bind
	device.tun.device = tunDevice
	mtu, err := device.tun.device.MTU()
	if err != nil {
		device.log.Errorf("Trouble determining MTU, assuming default: %v", err)
		mtu = DefaultMTU
	}
	device.tun.mtu.Store(int32(mtu))
	device.tun.queues = tun.QueuesOf(tunDevice)
	device.tun.writeTo = tun.WriteToOf(tunDevice)
	device.peers.keyMap = make(map[NoisePublicKey]*Peer)
	device.rate.limiter.Init()
	device.indexTable.Init()

	if want := len(device.tun.queues); want > device.config.queueStagedSize {
		device.log.Errorf(
			"Raising staged queue size to fit concurrent tun readers: %d -> %d",
			device.config.queueStagedSize,
			want,
		)
		device.config.queueStagedSize = want
	}

	device.PopulatePools()

	// create queues

	device.queue.handshake = newHandshakeQueue(device.config.queueHandshakeSize)
	// Scale to the number of producers
	device.queue.encryption = newOutboundQueue(
		device.config.queueOutboundSize * len(device.tun.queues),
	)
	device.queue.decryption = newInboundQueue(device.config.queueInboundSize)

	// start workers

	cpus := runtime.NumCPU()
	device.state.stopping.Wait()
	device.queue.encryption.wg.Add(cpus) // One for each RoutineHandshake
	for i := 0; i < cpus; i++ {
		go device.RoutineEncryption(i + 1)
		go device.RoutineDecryption(i + 1)
		go device.RoutineHandshake(i + 1)
	}

	device.state.stopping.Add(len(device.tun.queues))      // RoutineReadFromTUN
	device.queue.encryption.wg.Add(len(device.tun.queues)) // RoutineReadFromTUN
	for i, q := range device.tun.queues {
		go device.RoutineReadFromTUN(i, q)
	}
	go device.RoutineTUNEventReader()

	return device
}

// BatchSize returns the BatchSize for the device as a whole which is the max of
// the bind batch size and the tun batch size. The batch size reported by device
// is the size used to construct memory pools, and is the allowed batch size for
// the lifetime of the device.
func (device *Device) BatchSize() int {
	size := device.net.bind.BatchSize()
	dSize := device.tun.device.BatchSize()
	if size < dSize {
		size = dSize
	}
	return size
}

// LookupPeer looks up a peer by its public key.
//
// If the peer does not exist and a [PeerLookupFunc] is set (via
// [Device.SetPeerLookupFunc]), then that function is used to create the peer
// before returning it. Peers created via this mechanism exist only until their
// state machine reaches idle, and then the peers are removed.
//
// If the peer does not exist and no [PeerLookupFunc] is set, nil is returned.
//
// Use [Device.LookupActivePeer] to only return already-existing peers, without
// using a [PeerLookupFunc].
func (device *Device) LookupPeer(pk NoisePublicKey) *Peer {
	device.peers.RLock()
	p, ok := device.peers.keyMap[pk]
	lookupFunc := device.peers.lookupFunc
	device.peers.RUnlock()
	if ok || lookupFunc == nil {
		return p
	}

	conf, ok := lookupFunc(pk)
	if !ok || conf == nil {
		return nil
	}

	p, err := device.NewPeer(pk)
	if err != nil {
		if errors.Is(err, errAddExistingPeer) {
			device.peers.RLock()
			defer device.peers.RUnlock()
			return device.peers.keyMap[pk]
		}
		device.log.Errorf("Failed to create peer: %v", err)
		return nil
	}
	p.SetPresharedKey(conf.PresharedKey)
	p.SetAllowedIPs(conf.AllowedIPs)
	p.deleteOnIdle = true
	if conf.Endpoint != nil {
		p.endpoint.Lock()
		p.endpoint.val = conf.Endpoint
		p.endpoint.Unlock()
	}
	p.Start()
	return p
}

// LookupActivePeer looks up a peer by its public key.
//
// Unlike [Device.LookupPeer], this function does not use a [PeerLookupFunc] to
// create the peer if it does not already exist.
//
// If the peer does not exist or was created lazily via [PeerLookupFunc]
// and has subsequently idled away, it returns (nil, false).
func (device *Device) LookupActivePeer(pk NoisePublicKey) (_ *Peer, ok bool) {
	device.peers.RLock()
	defer device.peers.RUnlock()
	p, ok := device.peers.keyMap[pk]
	return p, ok
}

var errAddExistingPeer = errors.New("adding existing peer")

func (device *Device) RemovePeer(key NoisePublicKey) {
	device.peers.Lock()
	defer device.peers.Unlock()
	// stop peer and remove from routing

	peer, ok := device.peers.keyMap[key]
	if ok {
		removePeerLocked(device, peer, key)
	}
}

func (device *Device) RemoveAllPeers() {
	device.peers.Lock()
	defer device.peers.Unlock()

	for key, peer := range device.peers.keyMap {
		removePeerLocked(device, peer, key)
	}

	device.peers.keyMap = make(map[NoisePublicKey]*Peer)
}

// RemoveMatchingPeers removes all peers for which shouldRemove returns true.
//
// It returns the number of peers removed.
func (device *Device) RemoveMatchingPeers(shouldRemove func(NoisePublicKey) bool) (numRemoved int) {
	device.peers.Lock()
	defer device.peers.Unlock()

	for key, peer := range device.peers.keyMap {
		if shouldRemove(key) {
			removePeerLocked(device, peer, key)
			numRemoved++
		}
	}
	return numRemoved
}

// NewPeerConfig are the configuration parameters for a new peer created via a
// [PeerLookupFunc] func.
type NewPeerConfig struct {
	// AllowedIPs is the initial set of allowed IPs for the new peer.
	AllowedIPs []netip.Prefix

	// PresharedKey is the initial pre-shared key for the new peer. The zero
	// value disables the optional WireGuard pre-shared-key layer.
	PresharedKey NoisePresharedKey

	// Endpoint, if non-nil, sets the endpoint for newly created peers.
	// The endpoint is pinned for the lifetime of the peer.
	Endpoint conn.Endpoint
}

// PeerLookupFunc is the type of function used to look up peers by public key
// when receiving packets for unknown peers.
//
// If it returns nil, the peer is not known.
//
// Otherwise, returning non-nil signals that wireguard-go should create the peer
// with the provided allowed IPs.
//
// See [Device.SetPeerLookupFunc] and [Device.LookupPeer].
type PeerLookupFunc func(NoisePublicKey) (_ *NewPeerConfig, ok bool)

// PeerByIPPacketFunc is the type of function used to look up a peer to send to
// for a given src/dst IP pair. The ipPkt parameter is the raw IP packet being
// routed; callers needing transport-layer ports or other header fields may parse
// them from ipPkt, but must handle IP fragmentation (ports may be absent on
// non-first fragments) and protocols that do not use ports (e.g. ICMP).
//
// Except for experimental use cases, dst is the only address
// that should be relied upon when looking up a peer.
//
// If it returns ok=false, the peer is not known.
//
// See [Device.SetPeerByIPPacketFunc] and [Device.SetPeerLookupFunc].
type PeerByIPPacketFunc func(src, dst netip.Addr, ipPkt []byte) (_ NoisePublicKey, ok bool)

// PeerSessionState is the current WireGuard session state for a peer.
type PeerSessionState uint8

const (
	// PeerSessionNone means there is no handshake in progress and no session key
	// material retained for this peer.
	PeerSessionNone PeerSessionState = iota

	// PeerSessionHandshake means a handshake is in progress for this peer, but
	// there is not currently a usable WireGuard session.
	PeerSessionHandshake

	// PeerSessionEstablished means the peer has a completed WireGuard session
	// with usable session key material.
	PeerSessionEstablished

	// PeerSessionExpired means the peer's session key material is no longer
	// considered usable, but final key cleanup or lazy peer removal may not have
	// happened yet.
	PeerSessionExpired
)

// PeerSessionStateFunc is called when a peer's WireGuard session state changes.
//
// Calls are serialized per peer and delivered in that peer's transition order. The
// callback must be cheap and must not call back into Device.
type PeerSessionStateFunc func(peer NoisePublicKey, state PeerSessionState)

// SetPeerLookupFunc sets the function used to look up peers by public key
// when receiving packets for unknown peers.
func (device *Device) SetPeerLookupFunc(f PeerLookupFunc) {
	device.peers.Lock()
	defer device.peers.Unlock()
	device.peers.lookupFunc = f
}

// SetPeerByIPPacketFunc sets the function used to look up peers by IP address
// when sending packets to unknown peers.
func (device *Device) SetPeerByIPPacketFunc(f PeerByIPPacketFunc) {
	device.allowedips.mu.Lock()
	defer device.allowedips.mu.Unlock()
	device.allowedips.peerByIPPacketFunc = f
	device.allowedips.device = device
}

// SetSessionStateFunc sets the function used to observe peer WireGuard session
// state changes.
//
// It does not replay current state. Callers that need a complete view should set
// it before peers are started or lazily created, and maintain any snapshots,
// sequence numbers, and pubsub state outside wireguard-go.
//
// The callback must be concurrent-safe and must not call back into Device.
func (device *Device) SetSessionStateFunc(f PeerSessionStateFunc) {
	if f == nil {
		device.peerStateFn.Store(nil)
		return
	}
	device.peerStateFn.Store(&f)
}

// MaxPriorityMessageContentSize is the maximum size of a message returned by a
// [PeerPriorityMessageFunc]. It's a power of 2 that leaves significant space
// when accounting for all WireGuard overhead and encapsulating network protocol
// headers. Future adjustments to this value should consider all these overheads
// and any [conn.Bind] implementation limitations.
const MaxPriorityMessageContentSize = 512

// PeerPriorityMessageFunc is called when a peer's WireGuard session keypair is
// established (or re-keyed) for forward data transmission.
//
// The returned message is transmitted to the peer in priority fashion. Priority
// means it cannot be evicted from the staged packet queue by non-priority
// (read from [tun.Device]) packets. It avoids the staged queue altogether.
//
// The callback must be cheap and must not call back into [Device]. A zero length
// message or a message whose length exceeds [MaxPriorityMessageContentSize] will
// be silently dropped. Message should start with an IPv4 or IPv6 header as it
// is subject to allowed IPs lookup on the receiver, same as any other transport
// message.
type PeerPriorityMessageFunc func(peer NoisePublicKey) (msg []byte)

// SetPriorityMessageOnEstablishmentFunc sets a function to be used for sending
// a priority message around session establishment. See [PeerPriorityMessageFunc]
// docs for more details. A nil value clears any previously set value.
func (device *Device) SetPriorityMessageOnEstablishmentFunc(f PeerPriorityMessageFunc) {
	if f == nil {
		device.priorityMsgFn.Store(nil)
		return
	}
	device.priorityMsgFn.Store(&f)
}

// ScheduleHandshakeOnUserSend marks the peer for handshake initiation immediately
// following its next outbound transport message. It is a no-op if the peer is
// unknown or currently holds no key material.
//
// The initiation remains subject to [RekeyTimeout] elapsing since the last
// handshake message was sent. A request blocked by that window stays armed for
// a later transport message rather than being dropped.
func (device *Device) ScheduleHandshakeOnUserSend(peer NoisePublicKey) {
	device.peers.RLock()
	defer device.peers.RUnlock()
	p, ok := device.peers.keyMap[peer]
	if !ok {
		return
	}
	if !p.hasKeyMaterial() {
		return
	}
	p.handshakeOnUserSend.Store(true)
}

func (device *Device) Close() {
	device.state.Lock()
	defer device.state.Unlock()
	device.ipcMutex.Lock()
	defer device.ipcMutex.Unlock()
	if device.isClosed() {
		return
	}
	device.state.state.Store(uint32(deviceStateClosed))
	device.log.Verbosef("Device closing")

	device.tun.device.Close()
	device.downLocked()

	// Remove peers before closing queues,
	// because peers assume that queues are active.
	device.RemoveAllPeers()

	// We kept a reference to the encryption and decryption queues,
	// in case we started any new peers that might write to them.
	// No new peers are coming; we are done with these queues.
	device.queue.encryption.wg.Done()
	device.queue.decryption.wg.Done()
	device.queue.handshake.wg.Done()
	device.state.stopping.Wait()

	device.rate.limiter.Close()

	device.log.Verbosef("Device closed")
	close(device.closed)
}

func (device *Device) Wait() chan struct{} {
	return device.closed
}

func (device *Device) SendKeepalivesToPeersWithCurrentKeypair() {
	if !device.isUp() {
		return
	}

	// Collect the set of peers to keepalive under peers.RLock, then release
	// before invoking SendKeepalive. SendKeepalive can reach
	// CreateMessageInitiation which acquires staticIdentity.RLock; holding
	// peers.RLock across that path would invert the
	// staticIdentity < peers hierarchy (see lock-ordering.md).
	var peers []*Peer
	device.peers.RLock()
	for _, peer := range device.peers.keyMap {
		peer.keypairs.RLock()
		sendKeepalive := peer.keypairs.current != nil && !peer.keypairs.current.created.Add(RejectAfterTime).Before(time.Now())
		peer.keypairs.RUnlock()
		if sendKeepalive {
			peers = append(peers, peer)
		}
	}
	device.peers.RUnlock()
	for _, peer := range peers {
		peer.SendKeepalive()
	}
}

// closeBindLocked closes the device's net.bind.
// The caller must hold the net mutex.
func closeBindLocked(device *Device) error {
	var err error
	netc := &device.net
	if netc.netlinkCancel != nil {
		netc.netlinkCancel.Cancel()
	}
	if netc.bind != nil {
		err = netc.bind.Close()
	}
	netc.stopping.Wait()
	return err
}

func (device *Device) Bind() conn.Bind {
	device.net.Lock()
	defer device.net.Unlock()
	return device.net.bind
}

func (device *Device) BindSetMark(mark uint32) error {
	device.net.Lock()
	defer device.net.Unlock()

	// check if modified
	if device.net.fwmark == mark {
		return nil
	}

	// update fwmark on existing bind
	device.net.fwmark = mark
	if device.isUp() && device.net.bind != nil {
		if err := device.net.bind.SetMark(mark); err != nil {
			return err
		}
	}

	// clear cached source addresses
	device.peers.RLock()
	for _, peer := range device.peers.keyMap {
		peer.markEndpointSrcForClearing()
	}
	device.peers.RUnlock()

	return nil
}

func (device *Device) BindUpdate() error {
	device.net.Lock()
	defer device.net.Unlock()

	// close existing sockets
	if err := closeBindLocked(device); err != nil {
		return err
	}

	// open new sockets
	if !device.isUp() {
		return nil
	}

	// bind to new port
	var err error
	var recvFns []conn.ReceiveFunc
	netc := &device.net

	recvFns, netc.port, err = netc.bind.Open(netc.port)
	if err != nil {
		netc.port = 0
		return err
	}

	netc.netlinkCancel, err = device.startRouteListener(netc.bind)
	if err != nil {
		netc.bind.Close()
		netc.port = 0
		return err
	}

	// set fwmark
	if netc.fwmark != 0 {
		err = netc.bind.SetMark(netc.fwmark)
		if err != nil {
			return err
		}
	}

	// clear cached source addresses
	device.peers.RLock()
	for _, peer := range device.peers.keyMap {
		peer.markEndpointSrcForClearing()
	}
	device.peers.RUnlock()

	// start receiving routines
	device.net.stopping.Add(len(recvFns))
	device.queue.decryption.wg.Add(len(recvFns)) // each RoutineReceiveIncoming goroutine writes to device.queue.decryption
	device.queue.handshake.wg.Add(len(recvFns))  // each RoutineReceiveIncoming goroutine writes to device.queue.handshake
	batchSize := netc.bind.BatchSize()
	for _, fn := range recvFns {
		go device.RoutineReceiveIncoming(batchSize, fn)
	}

	device.log.Verbosef("UDP bind has been updated")
	return nil
}

func (device *Device) BindClose() error {
	device.net.Lock()
	err := closeBindLocked(device)
	device.net.Unlock()
	return err
}
