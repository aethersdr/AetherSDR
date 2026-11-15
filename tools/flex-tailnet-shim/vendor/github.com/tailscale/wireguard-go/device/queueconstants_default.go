//go:build !android && !ios && !windows

/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2017-2023 WireGuard LLC. All Rights Reserved.
 */

package device

import "github.com/tailscale/wireguard-go/conn"

const (
	DefaultQueueStagedSize            = conn.IdealBatchSize
	DefaultQueueOutboundSize          = 1024
	DefaultQueueInboundSize           = 1024
	DefaultQueueHandshakeSize         = 1024
	MaxSegmentSize                    = (1 << 16) - 1 // largest possible UDP datagram
	DefaultPreallocatedBuffersPerPool = 0             // Disable and allow for infinite memory growth
)
