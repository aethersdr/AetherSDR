/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2017-2023 WireGuard LLC. All Rights Reserved.
 */

package tun

import (
	"os"
)

type Event int

const (
	EventUp = 1 << iota
	EventDown
	EventMTUUpdate
)

// ReadPacket describes a packet read by [Reader.Read].
type ReadPacket struct {
	// Offset is the starting byte offset.
	Offset int
	// Size is the size of the packet.
	Size int
}

// ReadPacketSpacing is the number of bytes reserved before the first packet,
// between adjacent packets, and after the final packet filled by [Reader.Read].
const ReadPacketSpacing = 64

// Reader reads packets from a single queue of a [Device].
type Reader interface {
	// Read reads one or more packets from the queue into slab. On return, it
	// populates packets and returns the number of entries to evaluate. Those
	// entries are valid even when err is non-nil. Callers must provide at least
	// [Device.BatchSize] entries.
	//
	// Read reserves [ReadPacketSpacing] bytes before the first packet, between
	// adjacent packets, and after the final packet. The contents of the reserved
	// space are unspecified; the [Device] may use them while Read is executing.
	// Callers must provide a slab of at least 2*[ReadPacketSpacing] bytes.
	//
	// Read returns [ErrTooManySegments] if packets or slab cannot accommodate
	// all packets produced by the read.
	Read(slab []byte, packets []ReadPacket) (n int, err error)
}

// Queue is a single read queue of a [Device]. Distinct Queues of one Device
// may be used concurrently.
//
// A [Device] implementation may return read-only Queues in addition to those
// backed by file descriptors. [MultiQueueDevice.WriteTo] is the way to
// dispatch writes to the queues that accept them.
type Queue interface {
	Reader

	// File returns the queue's file descriptor, or nil if the queue is not
	// backed by one.
	File() *os.File
}

type Device interface {
	// Queue is the Device's first queue. A [MultiQueueDevice] exposes the
	// rest.
	Queue

	// Write one or more packets to the Device.
	// On a successful write it returns the number of packets written. A nonzero
	// offset can be used to instruct the Device on where to begin writing from
	// each packet contained within the bufs slice.
	Write(bufs [][]byte, offset int) (int, error)

	// MTU returns the MTU of the Device.
	MTU() (int, error)

	// Name returns the current name of the Device.
	Name() (string, error)

	// Events returns a channel of type Event, which is fed Device events.
	Events() <-chan Event

	// Close stops the Device and closes the Event channel.
	Close() error

	// BatchSize returns the preferred/max number of packets that can be read or
	// written in a single read/write call. BatchSize must not change over the
	// lifetime of a Device.
	BatchSize() int
}

// MultiQueueDevice is a Device that exposes more than one kernel queue, each
// backed by its own file descriptor. The count is fixed for the Device's
// lifetime.
type MultiQueueDevice interface {
	Device

	// Queues returns one [Queue] per kernel queue with at least one entry.
	// Entry 0 is equivalent to the Device's own [Reader.Read].
	Queues() []Queue

	// WriteTo is [Device.Write] directed at one of the Device's write queues.
	// Data written with the same flow id lands on the same write queue.
	WriteTo(flow int, bufs [][]byte, offset int) (int, error)
}

// WriteToOf returns dev's queue-aware write, or [Device.Write] for a Device
// with one write queue.
func WriteToOf(dev Device) func(flow int, bufs [][]byte, offset int) (int, error) {
	if w, ok := dev.(MultiQueueDevice); ok {
		return w.WriteTo
	}
	return func(_ int, bufs [][]byte, offset int) (int, error) {
		return dev.Write(bufs, offset)
	}
}

// QueuesOf returns dev's queues, or dev itself as the single queue. The result
// always has at least one entry.
func QueuesOf(dev Device) []Queue {
	if mq, ok := dev.(MultiQueueDevice); ok {
		if qs := mq.Queues(); len(qs) > 0 {
			return qs
		}
	}
	return []Queue{dev}
}

// An Option configures a [Device] at creation time.
type Option interface {
	apply(*config)
}

type optionFunc func(*config)

func (f optionFunc) apply(config *config) {
	f(config)
}

type config struct {
	queues int // Tun fd's in IFF_MULTI_QUEUE group to open.
}

func defaultConfig() config {
	return config{
		queues: 1,
	}
}

// WithQueues requests that the [Device] be created with n queues. n must be at
// least 1.
//
// Only Linux implements multiqueue TUN. Errors are returned if more queues
// are requested than the Linux kernel supports (at the time of writing,
// v7.0 has MAX_TAP_QUEUES at 256) or if the per-process limit was reached.
//
// Implementations may choose to return more read queues than was requested.
// Callers must consult [QueuesOf] rather than assume they got n.
//
// EXPERIMENTAL: Current implementation introduces nonce reordering with more
// than one queue.
func WithQueues(n int) Option {
	return optionFunc(func(config *config) {
		config.queues = n
	})
}

// GRODevice is a Device extended with methods for disabling GRO. Certain OS
// versions may have offload bugs. Where these bugs negatively impact throughput
// or break connectivity entirely we can use these methods to disable the
// related offload.
//
// Linux has the following known, GRO bugs.
//
// torvalds/linux@e269d79c7d35aa3808b1f3c1737d63dab504ddc8 broke virtio_net
// TCP & UDP GRO causing GRO writes to return EINVAL. The bug was then
// resolved later in
// torvalds/linux@89add40066f9ed9abe5f7f886fe5789ff7e0c50e. The offending
// commit was pulled into various LTS releases.
//
// UDP GRO writes end up blackholing/dropping packets destined for a
// vxlan/geneve interface on kernel versions prior to 6.8.5.
type GRODevice interface {
	Device
	// DisableUDPGRO disables UDP GRO if it is enabled.
	DisableUDPGRO()
	// DisableTCPGRO disables TCP GRO if it is enabled.
	DisableTCPGRO()
}
