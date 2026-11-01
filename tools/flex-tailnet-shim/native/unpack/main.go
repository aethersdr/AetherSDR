//go:build unix

// Command unpack decompresses the gzipped shim into RAM at each start. A
// FLEX-6700's /user partition, where legacy waveforms live, has about 13 MB
// free, and the shim is 20 MB uncompressed; shipping it gzipped keeps the
// package under 10 MB there. It needs nothing from the radio but the Go
// runtime the shim needs anyway.
//
//	unpack SRC.gz DST
//
// DST's directory is shared RAM (/tmp), so it must be a real directory that
// this user owns and nobody else can write or read: otherwise someone else's
// directory could swap the binary before it runs.
package main

import (
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"syscall"
)

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: unpack SRC.gz DST")
		os.Exit(2)
	}
	if err := unpack(os.Args[1], os.Args[2]); err != nil {
		fmt.Fprintln(os.Stderr, "unpack:", err)
		os.Exit(1)
	}
}

// unpack writes src's contents to dst atomically and executable. A
// truncated or corrupt src fails the gzip checksum and leaves dst alone.
func unpack(src, dst string) error {
	f, err := os.Open(src)
	if err != nil {
		return err
	}
	defer f.Close()
	zr, err := gzip.NewReader(f)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0o700); err != nil {
		return err
	}
	if err := privateDir(filepath.Dir(dst)); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(dst), ".unpack-*")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name()) // a no-op once renamed
	if _, err := io.Copy(tmp, zr); err != nil {
		tmp.Close()
		return err
	}
	if err := zr.Close(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Chmod(0o755); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(tmp.Name(), dst)
}

// privateDir refuses dir unless it is a directory (not a symlink) owned by
// this user with no group or other access.
func privateDir(dir string) error {
	fi, err := os.Lstat(dir)
	if err != nil {
		return err
	}
	st, ok := fi.Sys().(*syscall.Stat_t)
	switch {
	case !fi.IsDir():
		return fmt.Errorf("%s is not a directory", dir)
	case !ok || int(st.Uid) != os.Getuid():
		return fmt.Errorf("%s belongs to another user", dir)
	case fi.Mode().Perm()&0o077 != 0:
		return fmt.Errorf("%s is open to other users (mode %v)", dir, fi.Mode().Perm())
	}
	return nil
}
