/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2017-2023 WireGuard LLC. All Rights Reserved.
 */

package device

import (
	"sync"

	"github.com/tailscale/wireguard-go/tun"
)

type WaitPool struct {
	pool  sync.Pool
	cond  sync.Cond
	lock  sync.Mutex
	count uint32 // Get calls not yet Put back
	max   uint32
}

func NewWaitPool(max uint32, new func() any) *WaitPool {
	p := &WaitPool{pool: sync.Pool{New: new}, max: max}
	p.cond = sync.Cond{L: &p.lock}
	return p
}

func (p *WaitPool) Get() any {
	if p.max != 0 {
		p.lock.Lock()
		for p.count >= p.max {
			p.cond.Wait()
		}
		p.count++
		p.lock.Unlock()
	}
	return p.pool.Get()
}

func (p *WaitPool) Put(x any) {
	p.pool.Put(x)
	if p.max == 0 {
		return
	}
	p.lock.Lock()
	defer p.lock.Unlock()
	p.count--
	p.cond.Signal()
}

const (
	// smallPacketBufSize is the size to be used for [packetBuf.slab]'s pooled in
	// [Device.pool.smallPacketBufs]. It's large enough to fit WireGuard keepalive
	// messages, WireGuard-encrypted [MaxPriorityMessageContentSize], and
	// WireGuard-encrypted internet MTU messages.
	smallPacketBufSize = 2048

	internetMTU = 1500
	// assert that [smallPacketBufSize] is sufficient to hold [internetMTU]-sized
	// plaintext read via [tun.Device.Read], for eventual encryption and
	// transmission as a WireGuard transport message.
	_ uint = smallPacketBufSize -
		tun.ReadPacketSpacing -
		internetMTU -
		outboundPlaintextTailroom
	// assert that [smallPacketBufSize] is sufficient to hold [MaxPriorityMessageContentSize]-sized
	// plaintext for eventual encryption and transmission as a WireGuard transport
	// message. This assertion also covers keepalive messages, which only have
	// the auth tag, accounted for in [outboundPlaintextTailroom], following
	// the WireGuard header.
	_ uint = smallPacketBufSize -
		outboundPlaintextHeadroom -
		MaxPriorityMessageContentSize -
		outboundPlaintextTailroom

	// minPacketBufSizeForDistinctSmallPool represents the minimum [packetBuf.slab]
	// size at which a distinct ([Device.pool.smallPacketBufs]) is created.
	minPacketBufSizeForDistinctSmallPool = 2 * smallPacketBufSize
)

func (device *Device) PopulatePools() {
	device.pool.inboundElementsContainer = NewWaitPool(device.config.preallocatedBuffersPerPool, func() any {
		s := make([]*QueueInboundElement, 0, device.BatchSize())
		return &QueueInboundElementsContainer{elems: s}
	})
	device.pool.outboundElementsContainer = NewWaitPool(device.config.preallocatedBuffersPerPool, func() any {
		s := make([]*QueueOutboundElement, 0, device.BatchSize())
		return &QueueOutboundElementsContainer{elems: s}
	})
	device.pool.inboundElements = NewWaitPool(device.config.preallocatedBuffersPerPool, func() any {
		return new(QueueInboundElement)
	})
	device.pool.outboundElements = NewWaitPool(device.config.preallocatedBuffersPerPool, func() any {
		return new(QueueOutboundElement)
	})
	packetBufSize := singlePacketSlabSize
	if device.BatchSize() > 1 {
		packetBufSize = batchingSlabSize
	}
	device.pool.packetBufs = NewWaitPool(device.config.preallocatedBuffersPerPool, func() any {
		return newPacketBuf(packetBufSize, func(buf *packetBuf) {
			device.pool.packetBufs.Put(buf)
		})
	})
	// A distinct smallPacketBufs pool makes TUN readers copy packets that fit
	// into [smallPacketBufSize] in the interest of memory savings. Alias
	// smallPacketBufs to the "full-sized" pool if there is little to be gained
	// in memory savings, or if the device is configured to limit outstanding
	// packet memory pool items, which we don't want to artificially inflate.
	device.pool.smallPacketBufs = device.pool.packetBufs
	if packetBufSize >= minPacketBufSizeForDistinctSmallPool && device.config.preallocatedBuffersPerPool == 0 {
		device.pool.smallPacketBufs = NewWaitPool(device.config.preallocatedBuffersPerPool, func() any {
			return newPacketBuf(smallPacketBufSize, func(buf *packetBuf) {
				device.pool.smallPacketBufs.Put(buf)
			})
		})
	}
}

func (device *Device) GetInboundElementsContainer() *QueueInboundElementsContainer {
	c := device.pool.inboundElementsContainer.Get().(*QueueInboundElementsContainer)
	return c
}

func (device *Device) PutInboundElementsContainer(c *QueueInboundElementsContainer) {
	for i := range c.elems {
		c.elems[i] = nil
	}
	c.elems = c.elems[:0]
	device.pool.inboundElementsContainer.Put(c)
}

func (device *Device) GetOutboundElementsContainer() *QueueOutboundElementsContainer {
	c := device.pool.outboundElementsContainer.Get().(*QueueOutboundElementsContainer)
	return c
}

func (device *Device) PutOutboundElementsContainer(c *QueueOutboundElementsContainer) {
	for i := range c.elems {
		c.elems[i] = nil
	}
	c.elems = c.elems[:0]
	device.pool.outboundElementsContainer.Put(c)
}

// hasDistinctSmallPacketBufPool returns true if [Device.getSmallPacketBuf]
// will fetch from a pool independent of [Device.getPacketBuf].
func (device *Device) hasDistinctSmallPacketBufPool() bool {
	return device.pool.smallPacketBufs != device.pool.packetBufs
}

// getSmallPacketBuf returns a [*packetBuf] smaller or equal in size to what
// [Device.getPacketBuf] returns depending on [Device] batching and [WaitPool]
// configuration.
func (device *Device) getSmallPacketBuf() *packetBuf {
	b := device.pool.smallPacketBufs.Get().(*packetBuf)
	b.incRef()
	return b
}

func (device *Device) getPacketBuf() *packetBuf {
	b := device.pool.packetBufs.Get().(*packetBuf)
	b.incRef()
	return b
}

func (device *Device) GetInboundElement() *QueueInboundElement {
	return device.pool.inboundElements.Get().(*QueueInboundElement)
}

func (device *Device) PutInboundElement(elem *QueueInboundElement) {
	elem.buffer.decRef()
	elem.clearPointers()
	device.pool.inboundElements.Put(elem)
}

// GetOutboundElement returns a [*QueueOutboundElement] with all its fields
// set to their respective zero values.
func (device *Device) GetOutboundElement() *QueueOutboundElement {
	elem := device.pool.outboundElements.Get().(*QueueOutboundElement)
	elem.plaintextOffset = 0
	elem.nonce = 0
	// buffer, packet, keypair, and peer were cleared (if necessary) by [QueueOutboundElement.clearPointers].
	return elem
}

func (device *Device) PutOutboundElement(elem *QueueOutboundElement) {
	if elem.buffer != nil {
		elem.buffer.decRef()
	}
	elem.clearPointers()
	device.pool.outboundElements.Put(elem)
}
