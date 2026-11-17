package main

import (
	"errors"
	"os"

	"golang.org/x/sys/unix"
)

func openNativeStateDirectory(path string) (*os.File, error) {
	fd, err := unix.Open(path, unix.O_RDONLY|unix.O_DIRECTORY|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0)
	if err != nil {
		return nil, &os.PathError{Op: "open native identity directory", Path: path, Err: err}
	}
	return os.NewFile(uintptr(fd), path), nil
}

func openNativeStateFile(path string, writing bool) (*os.File, error) {
	flags := unix.O_RDONLY | unix.O_NOFOLLOW | unix.O_NONBLOCK | unix.O_CLOEXEC
	if writing {
		flags = unix.O_WRONLY | unix.O_CREAT | unix.O_NOFOLLOW | unix.O_NONBLOCK | unix.O_CLOEXEC
	}
	fd, err := unix.Open(path, flags, 0600)
	return nativeStateFileDescriptor(fd, path, writing, err)
}

func openNativeStateFileAt(parent *os.File, name string) (*os.File, error) {
	fd, err := unix.Openat(int(parent.Fd()), name, unix.O_WRONLY|unix.O_CREAT|unix.O_NOFOLLOW|unix.O_NONBLOCK|unix.O_CLOEXEC, 0600)
	return nativeStateFileDescriptor(fd, name, true, err)
}

func renameNativeStateFileAt(parent *os.File, from, to string) error {
	return unix.Renameat(int(parent.Fd()), from, int(parent.Fd()), to)
}

func removeNativeStateFileAt(parent *os.File, name string) error {
	return unix.Unlinkat(int(parent.Fd()), name, 0)
}

func nativeStateFileDescriptor(fd int, path string, writing bool, err error) (*os.File, error) {
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
