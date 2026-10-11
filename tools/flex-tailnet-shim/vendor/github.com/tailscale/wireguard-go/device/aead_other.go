//go:build !arm && !(amd64 && gc && !purego)

/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2017-2023 WireGuard LLC. All Rights Reserved.
 */

package device

import (
	"crypto/cipher"

	"golang.org/x/crypto/chacha20poly1305"
)

func chacha20poly1305New(key []byte) (cipher.AEAD, error) {
	return chacha20poly1305.New(key)
}
