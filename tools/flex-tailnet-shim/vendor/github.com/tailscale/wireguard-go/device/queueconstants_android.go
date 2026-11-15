/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2017-2023 WireGuard LLC. All Rights Reserved.
 */

package device

import "github.com/tailscale/wireguard-go/conn"

/* Reduce memory consumption for Android */

const (
	DefaultQueueStagedSize            = conn.IdealBatchSize
	DefaultQueueOutboundSize          = 1024
	DefaultQueueInboundSize           = 1024
	DefaultQueueHandshakeSize         = 1024
	MaxSegmentSize                    = 2200
	DefaultPreallocatedBuffersPerPool = 4096
)
