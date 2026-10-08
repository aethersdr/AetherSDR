// Copyright (c) Tailscale Inc & AUTHORS
// SPDX-License-Identifier: BSD-3-Clause

//go:build windows && !386

package wingoes

import (
	"syscall"
	"unsafe"
)

func verSetConditionMask(conditionMask uint64, typeMask uint32, condition byte) uint64 {
	r0, _, _ := syscall.SyscallN(
		procVerSetConditionMask.Addr(),
		uintptr(conditionMask),
		uintptr(typeMask),
		uintptr(condition),
	)
	return uint64(r0)
}

func verifyVersionInfo(versionInformation *_OSVERSIONINFOEX, typeMask uint32, conditionMask uint64) (err error) {
	rc, _, e := syscall.SyscallN(
		procVerifyVersionInfo.Addr(),
		uintptr(unsafe.Pointer(versionInformation)),
		uintptr(typeMask),
		uintptr(conditionMask),
	)
	if int32(rc) == 0 {
		err = e
	}
	return err
}
