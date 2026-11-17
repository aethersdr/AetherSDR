package main

import (
	"errors"
	"os"

	"golang.org/x/sys/unix"
)

func openNativeStateFile(path string, writing bool) (*os.File, error) {
	flags := unix.O_RDONLY | unix.O_NOFOLLOW | unix.O_NONBLOCK | unix.O_CLOEXEC
	if writing {
		flags = unix.O_WRONLY | unix.O_CREAT | unix.O_NOFOLLOW | unix.O_NONBLOCK | unix.O_CLOEXEC
	}
	fd, err := unix.Open(path, flags, 0600)
	if err != nil {
		return nil, &os.PathError{Op: "open native identity", Path: path, Err: err}
	}
	var st unix.Stat_t
	err = unix.Fstat(fd, &st)
	if err == nil && (st.Mode&unix.S_IFMT != unix.S_IFREG || writing && (st.Nlink != 1 || st.Uid != uint32(os.Geteuid()))) {
		err = errors.New("native identity must be an owned regular file with one link")
	}
	if err != nil {
		unix.Close(fd)
		return nil, err
	}
	return os.NewFile(uintptr(fd), path), nil
}
