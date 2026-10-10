/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2017-2023 WireGuard LLC. All Rights Reserved.
 */

package device

const (
	DefaultQueueStagedSize            = 128
	DefaultQueueOutboundSize          = 1024
	DefaultQueueInboundSize           = 1024
	DefaultQueueHandshakeSize         = 1024
	MaxSegmentSize                    = 2048 - 32 // largest possible UDP datagram
	DefaultPreallocatedBuffersPerPool = 0         // Disable and allow for infinite memory growth
)
