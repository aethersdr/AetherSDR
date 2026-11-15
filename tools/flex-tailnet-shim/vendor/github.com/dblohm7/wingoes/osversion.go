// Copyright (c) 2022 Tailscale Inc & AUTHORS. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

//go:build windows

package wingoes

import (
	"fmt"
	"sync"
	"sync/atomic"
	"unsafe"

	"golang.org/x/sys/windows"
	"golang.org/x/sys/windows/registry"
)

const (
	_VER_MINORVERSION     = 0x0000001
	_VER_MAJORVERSION     = 0x0000002
	_VER_BUILDNUMBER      = 0x0000004
	_VER_PLATFORMID       = 0x0000008
	_VER_SERVICEPACKMINOR = 0x0000010
	_VER_SERVICEPACKMAJOR = 0x0000020
	_VER_SUITENAME        = 0x0000040
	_VER_PRODUCT_TYPE     = 0x0000080
)

const (
	_VER_EQUAL         = 1
	_VER_GREATER       = 2
	_VER_GREATER_EQUAL = 3
	_VER_LESS          = 4
	_VER_LESS_EQUAL    = 5
	_VER_AND           = 6
	_VER_OR            = 7
)

const (
	_VER_NT_WORKSTATION       = 1
	_VER_NT_DOMAIN_CONTROLLER = 2
	_VER_NT_SERVER            = 3
)

// The definition in x/sys/windows doesn't export the OSVersionInfoSize field,
// which we need to set before making API calls.
type _OSVERSIONINFOEX struct {
	OSVersionInfoSize uint32
	MajorVersion      uint32
	MinorVersion      uint32
	BuildNumber       uint32
	PlatformId        uint32
	CSDVersion        [128]uint16
	ServicePackMajor  uint16
	ServicePackMinor  uint16
	SuiteMask         uint16
	ProductType       byte
	_                 byte // Reserved
}

type osVersionInfo struct {
	fallbackBuildMin atomic.Uint32
	fallbackBuildMax atomic.Uint32
	build            uint32
	str              string
	isDC             bool
	isServer         bool
	useFallback      bool
}

var (
	// We cannot use mksyscall for these because they pass uint64 by value on
	// 32-bit CPU architectures.
	procVerSetConditionMask = modkernel32.NewProc("VerSetConditionMask")
	procVerifyVersionInfo   = modkernel32.NewProc("VerifyVersionInfoW")
	verOnce                 sync.Once
	verInfo                 osVersionInfo // must access via getVersionInfo()
)

func getVersionInfoInternal() (*_OSVERSIONINFOEX, error) {
	osv := _OSVERSIONINFOEX{
		OSVersionInfoSize: uint32(unsafe.Sizeof(_OSVERSIONINFOEX{})),
	}
	// Using GetVersionEx to account for app manifest.
	if err := getVersionEx(&osv); err != nil {
		return nil, err
	}
	return &osv, nil
}

func (osv *osVersionInfo) initFlags() {
	if isDC, err := verQueryProductType(_VER_NT_DOMAIN_CONTROLLER); err == nil {
		osv.isDC = isDC
		if isDC {
			// Domain Controllers are also implicitly servers.
			osv.isServer = true
			return
		}
	}

	if isServer, err := verQueryProductType(_VER_NT_SERVER); err == nil {
		osv.isServer = isServer
	}
}

func (osv *osVersionInfo) initFallback() {
	osv.initFlags()
	osv.useFallback = true
}

func verCheckManifest(osv *_OSVERSIONINFOEX) bool {
	major, minor, build := windows.RtlGetNtVersionNumbers()
	return major == osv.MajorVersion && minor == osv.MinorVersion && build == osv.BuildNumber
}

func (osv *osVersionInfo) init() {
	osvx, err := getVersionInfoInternal()
	if err != nil {
		osv.initFallback()
		return
	}

	// We only support 10.0.x.x
	if osvx.MajorVersion != 10 || osvx.MinorVersion != 0 {
		if verCheckManifest(osvx) {
			panic("change in versioning scheme -- package must be updated to interpret")
		} else {
			panic("incoherent Windows version -- missing/outdated manifest?")
		}
	}

	*osv = osVersionInfo{
		build: osvx.BuildNumber,
		str:   versionString(osvx),
		isDC:  osvx.ProductType == _VER_NT_DOMAIN_CONTROLLER,
		// Domain Controllers are also implicitly servers.
		isServer: osvx.ProductType == _VER_NT_DOMAIN_CONTROLLER || osvx.ProductType == _VER_NT_SERVER,
	}
}

func getVersionInfo() *osVersionInfo {
	verOnce.Do(verInfo.init)
	return &verInfo
}

func versionString(osv *_OSVERSIONINFOEX) string {
	fmtstr := "%d.%d.%d.%d"
	vers := append(make([]any, 0, 4),
		uint(osv.MajorVersion),
		uint(osv.MinorVersion),
		uint(osv.BuildNumber),
	)

	if ubr, err := getUBR(); err == nil {
		vers = append(vers, uint(ubr))
	} else {
		fmtstr = fmtstr[:len(fmtstr)-3]
	}

	return fmt.Sprintf(fmtstr, vers...)
}

// getUBR returns the "update build revision," ie. the fourth component of the
// version string found on Windows 10 and Windows 11 systems.
func getUBR() (uint32, error) {
	key, err := registry.OpenKey(registry.LOCAL_MACHINE,
		`SOFTWARE\Microsoft\Windows NT\CurrentVersion`, registry.QUERY_VALUE|registry.WOW64_64KEY)
	if err != nil {
		return 0, err
	}
	defer key.Close()

	val, valType, err := key.GetIntegerValue("UBR")
	if err != nil {
		return 0, err
	}
	if valType != registry.DWORD {
		return 0, registry.ErrUnexpectedType
	}

	return uint32(val), nil
}

// GetOSVersionString returns the Windows version of the current machine in
// dotted-decimal form.
func GetOSVersionString() string {
	return getVersionInfo().String()
}

// IsWinServer returns true if and only if this computer's version of Windows is
// a server edition.
func IsWinServer() bool {
	return getVersionInfo().isServer
}

// IsWinDomainController returs true if this computer's version of Windows is
// configured to act as a domain controller.
func IsWinDomainController() bool {
	return getVersionInfo().isDC
}

// Win10BuildConstant encodes build numbers for the various editions of Windows 10,
// for use with IsWin10BuildOrGreater.
type Win10BuildConstant uint32

const (
	Win10BuildRTM          = Win10BuildConstant(10240)
	Win10Build1511         = Win10BuildConstant(10586)
	Win10Build1607         = Win10BuildConstant(14393)
	WinServer2016          = Win10Build1607
	Win10BuildAnniversary  = Win10Build1607
	Win10Build1703         = Win10BuildConstant(15063)
	Win10BuildCreators     = Win10Build1703
	Win10Build1709         = Win10BuildConstant(16299)
	Win10BuildFallCreators = Win10Build1709
	Win10Build1803         = Win10BuildConstant(17134)
	Win10Build1809         = Win10BuildConstant(17763)
	WinServer2019          = Win10Build1809
	Win10Build1903         = Win10BuildConstant(18362)
	Win10Build1909         = Win10BuildConstant(18363)
	Win10Build2004         = Win10BuildConstant(19041)
	Win10Build20H2         = Win10BuildConstant(19042)
	Win10Build21H1         = Win10BuildConstant(19043)
	Win10Build21H2         = Win10BuildConstant(19044)
	Win10Build22H2         = Win10BuildConstant(19045)
	WinServer2022          = Win10BuildConstant(20348)
)

// IsWin10BuildOrGreater returns true when running on the specified Windows 10
// build, or newer.
func IsWin10BuildOrGreater(build Win10BuildConstant) bool {
	return getVersionInfo().isWin10BuildOrGreater(uint32(build))
}

// Win11BuildConstant encodes build numbers for the various editions of Windows 11,
// for use with IsWin11BuildOrGreater.
type Win11BuildConstant uint32

const (
	Win11BuildRTM  = Win11BuildConstant(22000)
	Win11Build22H2 = Win11BuildConstant(22621)
	Win11Build23H2 = Win11BuildConstant(22631)
	Win11Build24H2 = Win11BuildConstant(26100)
	WinServer2025  = Win11Build24H2
	Win11Build25H2 = Win11BuildConstant(26200)
	Win11Build26H1 = Win11BuildConstant(28000)
)

// IsWin11OrGreater returns true when running on any release of Windows 11,
// or newer.
func IsWin11OrGreater() bool {
	return IsWin11BuildOrGreater(Win11BuildRTM)
}

// IsWin11BuildOrGreater returns true when running on the specified Windows 11
// build, or newer.
func IsWin11BuildOrGreater(build Win11BuildConstant) bool {
	// Under the hood, Windows 11 is just Windows 10 with a sufficiently advanced
	// build number.
	return getVersionInfo().isWin10BuildOrGreater(uint32(build))
}

func (osv *osVersionInfo) String() string {
	return osv.str
}

func (osv *osVersionInfo) isWin10BuildOrGreater(build uint32) bool {
	if !osv.useFallback {
		return osv.build >= build
	}

	if osv.fallbackBuildMin.Load() >= build {
		return true
	}

	if build >= osv.fallbackBuildMax.Load() {
		return false
	}

	result, err := verQueryBuild(10, 0, build)
	if err == nil {
		if result {
			osv.fallbackBuildMin.Store(build)
		} else {
			osv.fallbackBuildMax.Store(build)
		}
	}
	return result
}

func verQueryBuild(major, minor, build uint32) (bool, error) {
	var condMask uint64
	condMask = verSetConditionMask(condMask, _VER_MAJORVERSION, _VER_GREATER_EQUAL)
	condMask = verSetConditionMask(condMask, _VER_MINORVERSION, _VER_GREATER_EQUAL)
	condMask = verSetConditionMask(condMask, _VER_BUILDNUMBER, _VER_GREATER_EQUAL)

	typeMask := uint32(_VER_MAJORVERSION | _VER_MINORVERSION | _VER_BUILDNUMBER)

	osv := _OSVERSIONINFOEX{
		MajorVersion: major,
		MinorVersion: minor,
		BuildNumber:  build,
	}

	return verVerify(&osv, typeMask, condMask)
}

func verQueryProductType(wantProdType byte) (bool, error) {
	condMask := verSetConditionMask(0, _VER_PRODUCT_TYPE, _VER_EQUAL)
	osv := _OSVERSIONINFOEX{
		ProductType: wantProdType,
	}
	return verVerify(&osv, _VER_PRODUCT_TYPE, condMask)
}

func verVerify(osv *_OSVERSIONINFOEX, typeMask uint32, condMask uint64) (bool, error) {
	osv.OSVersionInfoSize = uint32(unsafe.Sizeof(_OSVERSIONINFOEX{}))
	err := verifyVersionInfo(osv, typeMask, condMask)
	switch err {
	case nil:
		return true, nil
	case windows.ERROR_OLD_WIN_VERSION:
		return false, nil
	default:
		return false, err
	}
}
