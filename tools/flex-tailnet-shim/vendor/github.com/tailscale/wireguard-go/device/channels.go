/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2017-2023 WireGuard LLC. All Rights Reserved.
 */

package device

import (
	"sync"
)

// An outboundQueue is a channel of QueueOutboundElements awaiting encryption.
// An outboundQueue is ref-counted using its wg field.
// An outboundQueue created with newOutboundQueue has one reference.
// Every additional writer must call wg.Add(1).
// Every completed writer must call wg.Done().
// When no further writers will be added,
// call wg.Done to remove the initial reference.
// When the refcount hits 0, the queue's channel is closed.
type outboundQueue struct {
	c  chan *QueueOutboundElementsContainer
	wg sync.WaitGroup
}

func newOutboundQueue(capacity int) *outboundQueue {
	q := &outboundQueue{
		c: make(chan *QueueOutboundElementsContainer, capacity),
	}
	q.wg.Add(1)
	go func() {
		q.wg.Wait()
		close(q.c)
	}()
	return q
}

// A inboundQueue is similar to an outboundQueue; see those docs.
type inboundQueue struct {
	c  chan *QueueInboundElementsContainer
	wg sync.WaitGroup
}

func newInboundQueue(capacity int) *inboundQueue {
	q := &inboundQueue{
		c: make(chan *QueueInboundElementsContainer, capacity),
	}
	q.wg.Add(1)
	go func() {
		q.wg.Wait()
		close(q.c)
	}()
	return q
}

// A handshakeQueue is similar to an outboundQueue; see those docs.
type handshakeQueue struct {
	c  chan QueueHandshakeElement
	wg sync.WaitGroup
}

func newHandshakeQueue(capacity int) *handshakeQueue {
	q := &handshakeQueue{
		c: make(chan QueueHandshakeElement, capacity),
	}
	q.wg.Add(1)
	go func() {
		q.wg.Wait()
		close(q.c)
	}()
	return q
}
