//go:build !linux

package main

import (
	"errors"
	"os"
)

// Native radio builds use Linux's nonblocking, no-follow open. Keep the store
// available to desktop tooling without requiring Unix-only open flags.
func openNativeStateFile(path string, writing bool) (*os.File, error) {
	info, err := os.Lstat(path)
	if err != nil && !(writing && os.IsNotExist(err)) {
		return nil, err
	}
	if err == nil && !info.Mode().IsRegular() {
		return nil, errors.New("native identity is not a regular file")
	}
	flags := os.O_RDONLY
	if writing {
		flags = os.O_WRONLY | os.O_CREATE
	}
	return os.OpenFile(path, flags, 0600)
}
