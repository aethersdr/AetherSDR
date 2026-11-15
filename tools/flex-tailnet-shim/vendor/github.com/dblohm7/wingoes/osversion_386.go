// Copyright (c) Tailscale Inc & AUTHORS
// SPDX-License-Identifier: BSD-3-Clause

//go:build windows

package wingoes

import (
	"syscall"
	"unsafe"
)

func verSetConditionMask(conditionMask uint64, typeMask uint32, condition byte) uint64 {
	words := (*[2]uintptr)(unsafe.Pointer(&conditionMask))
	r0, r1, _ := syscall.SyscallN(
		procVerSetConditionMask.Addr(),
		words[0],
		words[1],
		uintptr(typeMask),
		uintptr(condition),
	)
	return uint64(r0) | (uint64(r1) << 32)
}

func verifyVersionInfo(versionInformation *_OSVERSIONINFOEX, typeMask uint32, conditionMask uint64) (err error) {
	words := (*[2]uintptr)(unsafe.Pointer(&conditionMask))
	rc, _, e := syscall.SyscallN(
		procVerifyVersionInfo.Addr(),
		uintptr(unsafe.Pointer(versionInformation)),
		uintptr(typeMask),
		words[0],
		words[1],
	)
	if int32(rc) == 0 {
		err = e
	}
	return err
}
