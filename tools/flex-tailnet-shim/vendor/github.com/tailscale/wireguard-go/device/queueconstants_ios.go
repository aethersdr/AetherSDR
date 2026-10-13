//go:build ios

/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2017-2023 WireGuard LLC. All Rights Reserved.
 */

package device

// Fit within memory limits for iOS's Network Extension API, which has stricter
// requirements. Heavier network extensions can reduce these further using
// [Device.Option]'s.
const (
	DefaultQueueStagedSize            = 128
	DefaultQueueOutboundSize          = 1024
	DefaultQueueInboundSize           = 1024
	DefaultQueueHandshakeSize         = 1024
	DefaultPreallocatedBuffersPerPool = 1024
)

const MaxSegmentSize = 1700
