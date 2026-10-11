// Copyright (c) Tailscale Inc & AUTHORS
// SPDX-License-Identifier: BSD-3-Clause

//go:build windows

package wingoes

//go:generate go run golang.org/x/sys/windows/mkwinsyscall -output zsyscall_windows.go mksyscall.go
//go:generate go run golang.org/x/tools/cmd/goimports -w zsyscall_windows.go

// Appending a '?' to this declaration because it's very, very deprecated, in
// the (admittedly unlikely) chance it actually gets removed in a future version
// of Windows.
//sys getVersionEx(osv *_OSVERSIONINFOEX) (err error) [int32(failretval)==0] = kernel32.GetVersionExW?
