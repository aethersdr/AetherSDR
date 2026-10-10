//go:build amd64 && gc && !purego

/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2017-2023 WireGuard LLC. All Rights Reserved.
 */

package device

import (
	"crypto/cipher"
	"os"

	"golang.org/x/crypto/chacha20poly1305"
	"golang.org/x/sys/cpu"

	asmAEAD "github.com/tailscale/wireguard-go/tsasm/amd64/chacha20poly1305"
)

/*
shouldUseTSAsm reports whether the data-path AEAD should come from tsasm/amd64. x/crypto gates its remaining amd64 kernel on HasSSSE3 && HasAVX2 && HasBMI2 and otherwise runs generic Go, so only CPUs with SSSE3 and no AVX2 are routed here. Pure so a test can cover every combination; the CPU variables cannot be written.
*/
func shouldUseTSAsm(hasSSSE3, hasAVX2, hasBMI2 bool) bool {
	return hasSSSE3 && !(hasAVX2 && hasBMI2)
}

var useTSAsm = shouldUseTSAsm(asmAEAD.Available(), cpu.X86.HasAVX2, cpu.X86.HasBMI2)

/*
chacha20poly1305New returns a ChaCha20-Poly1305 AEAD. On amd64 CPUs with SSSE3 but no AVX2 it uses the assembly kernel from tsasm/amd64/chacha20poly1305.

The cookie path (which uses the extended-nonce variant via chacha20poly1305.NewX) is left on the x/crypto path because it is not on the per-packet hot path.

As an escape hatch for hardware regressions or asm bugs, setting the environment variable TS_WG_ASM=0 forces the x/crypto implementation instead.
*/
func chacha20poly1305New(key []byte) (cipher.AEAD, error) {
	if !useTSAsm || os.Getenv("TS_WG_ASM") == "0" {
		return chacha20poly1305.New(key)
	}
	return asmAEAD.New(key)
}
