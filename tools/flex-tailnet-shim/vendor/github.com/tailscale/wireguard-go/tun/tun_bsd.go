//go:build freebsd || openbsd || darwin

/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2017-2023 WireGuard LLC. All Rights Reserved.
 */

package tun

import (
	"fmt"
	"os"
)

func createTUNFromFilesNoMQ(files []*os.File, mtu int) (Device, error) {
	if len(files) != 1 {
		for _, f := range files {
			f.Close()
		}
		return nil, fmt.Errorf("got %d TUN queue files: multiqueue TUN is only supported on Linux", len(files))
	}
	return CreateTUNFromFile(files[0], mtu)
}
