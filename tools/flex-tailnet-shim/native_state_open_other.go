//go:build !linux

package main

import (
	"errors"
	"os"
	"path/filepath"
)

func openNativeStateDirectory(path string) (*os.File, error) {
	info, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	if !info.IsDir() {
		return nil, errors.New("native identity parent is not a directory")
	}
	return os.Open(path)
}

func openNativeStateFileAt(parent *os.File, name string) (*os.File, error) {
	return openNativeStateFile(filepath.Join(parent.Name(), name), true)
}

func renameNativeStateFileAt(parent *os.File, from, to string) error {
	return os.Rename(filepath.Join(parent.Name(), from), filepath.Join(parent.Name(), to))
}

func removeNativeStateFileAt(parent *os.File, name string) error {
	return os.Remove(filepath.Join(parent.Name(), name))
}

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
