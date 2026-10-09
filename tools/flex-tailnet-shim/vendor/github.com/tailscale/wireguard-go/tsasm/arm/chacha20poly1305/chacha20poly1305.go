// SPDX-License-Identifier: BSD-3-Clause

//go:build arm

// Package chacha20poly1305 provides a ChaCha20-Poly1305 AEAD built
// from the hand-ported ChaCha20 and Poly1305 kernels in
// tsasm/arm/chacha20 and tsasm/arm/poly1305. It only builds on
// GOARCH=arm; other architectures should use
// golang.org/x/crypto/chacha20poly1305.
package chacha20poly1305

import (
	"crypto/cipher"
	"crypto/subtle"
	"encoding/binary"
	"errors"

	chacha "github.com/tailscale/wireguard-go/tsasm/arm/chacha20"
	"github.com/tailscale/wireguard-go/tsasm/arm/poly1305"
)

const (
	KeySize   = 32
	NonceSize = 12
	Overhead  = 16
)

type aead struct {
	key [KeySize]byte
}

// New returns a chacha20poly1305 AEAD with the given 32-byte key.
func New(key []byte) (cipher.AEAD, error) {
	if len(key) != KeySize {
		return nil, errors.New("chacha20poly1305: bad key length")
	}
	a := &aead{}
	copy(a.key[:], key)
	return a, nil
}

func (a *aead) NonceSize() int { return NonceSize }
func (a *aead) Overhead() int  { return Overhead }

func (a *aead) Seal(dst, nonce, plaintext, additionalData []byte) []byte {
	if len(nonce) != NonceSize {
		panic("chacha20poly1305: bad nonce length")
	}
	ret, out := sliceForAppend(dst, len(plaintext)+Overhead)

	var nonceArr [chacha.NonceSize]byte
	copy(nonceArr[:], nonce)

	// Derive the Poly1305 one-time key from the first 32 bytes of the
	// ChaCha20 keystream at counter 0, then encrypt at counter 1.
	var (
		zeros   [64]byte
		polyBuf [64]byte
		polyKey [32]byte
	)
	chacha.XORKeyStream(polyBuf[:], zeros[:], &a.key, &nonceArr, 0)
	copy(polyKey[:], polyBuf[:32])
	chacha.XORKeyStream(out[:len(plaintext)], plaintext, &a.key, &nonceArr, 1)

	tag := computeTag(additionalData, out[:len(plaintext)], &polyKey)
	copy(out[len(plaintext):], tag[:])
	return ret
}

func (a *aead) Open(dst, nonce, ciphertext, additionalData []byte) ([]byte, error) {
	if len(nonce) != NonceSize {
		return nil, errors.New("chacha20poly1305: bad nonce length")
	}
	if len(ciphertext) < Overhead {
		return nil, errors.New("chacha20poly1305: ciphertext too short")
	}
	ctLen := len(ciphertext) - Overhead
	ct := ciphertext[:ctLen]
	receivedTag := ciphertext[ctLen:]

	var nonceArr [chacha.NonceSize]byte
	copy(nonceArr[:], nonce)

	var (
		zeros   [64]byte
		polyBuf [64]byte
		polyKey [32]byte
	)
	chacha.XORKeyStream(polyBuf[:], zeros[:], &a.key, &nonceArr, 0)
	copy(polyKey[:], polyBuf[:32])

	expectedTag := computeTag(additionalData, ct, &polyKey)
	if subtle.ConstantTimeCompare(receivedTag, expectedTag[:]) != 1 {
		return nil, errors.New("chacha20poly1305: message authentication failed")
	}

	ret, out := sliceForAppend(dst, ctLen)
	chacha.XORKeyStream(out, ct, &a.key, &nonceArr, 1)
	return ret, nil
}

var zeros16 [16]byte

// computeTag streams (aad || pad16 || ct || pad16 || u64le(|aad|) ||
// u64le(|ct|)) through a stack-allocated Poly1305 MAC.
func computeTag(aad, ct []byte, polyKey *[32]byte) [16]byte {
	var mac poly1305.MAC
	mac.Init(polyKey)
	mac.Write(aad)
	if rem := len(aad) % 16; rem != 0 {
		mac.Write(zeros16[:16-rem])
	}
	mac.Write(ct)
	if rem := len(ct) % 16; rem != 0 {
		mac.Write(zeros16[:16-rem])
	}
	var lenBuf [16]byte
	binary.LittleEndian.PutUint64(lenBuf[0:8], uint64(len(aad)))
	binary.LittleEndian.PutUint64(lenBuf[8:16], uint64(len(ct)))
	mac.Write(lenBuf[:])

	var tag [16]byte
	mac.Sum(&tag)
	return tag
}

func sliceForAppend(dst []byte, n int) (head, tail []byte) {
	if total := len(dst) + n; cap(dst) >= total {
		head = dst[:total]
	} else {
		head = make([]byte, total)
		copy(head, dst)
	}
	tail = head[len(dst):]
	return
}
