//go:build linux

package main

import "golang.org/x/sys/unix"

// Only the fixed root-owned mirror is readable here. The shim never receives a
// privileged store descriptor or a command interface for transaction queries.
func readPDInstallationAt(root int, owner uint32) (pdInstallation, error) {
	fd, err := unix.FcntlInt(uintptr(root), unix.F_DUPFD_CLOEXEC, 3)
	if err != nil {
		return pdInstallation{}, errPDInstallationUnavailable
	}
	defer func() { unix.Close(fd) }()
	parts := []string{"run", "aetherpd", "installation"}
	for i := 0; ; i++ {
		var st unix.Stat_t
		if unix.Fstat(fd, &st) != nil || st.Uid != owner || st.Mode&07022 != 0 {
			return pdInstallation{}, errPDInstallationUnavailable
		}
		if i == len(parts) {
			var fs unix.Statfs_t
			if st.Mode&unix.S_IFMT != unix.S_IFREG || st.Mode&07777 != 0444 || st.Nlink != 1 || st.Size != pdInstallationSize || unix.Fstatfs(fd, &fs) != nil || fs.Type != unix.TMPFS_MAGIC {
				return pdInstallation{}, errPDInstallationUnavailable
			}
			data := make([]byte, pdInstallationSize)
			n, readErr := unix.Pread(fd, data, 0)
			if readErr != nil || n != len(data) {
				return pdInstallation{}, errPDInstallationUnavailable
			}
			return parsePDInstallation(data)
		}
		if st.Mode&unix.S_IFMT != unix.S_IFDIR {
			return pdInstallation{}, errPDInstallationUnavailable
		}
		flags := unix.O_RDONLY | unix.O_NOFOLLOW | unix.O_NONBLOCK | unix.O_CLOEXEC
		if i < len(parts)-1 {
			flags |= unix.O_DIRECTORY
		}
		next, openErr := unix.Openat(fd, parts[i], flags, 0)
		if openErr != nil {
			return pdInstallation{}, errPDInstallationUnavailable
		}
		unix.Close(fd)
		fd = next
	}
}

func readPDInstallation() (pdInstallation, error) {
	root, err := unix.Open("/", unix.O_RDONLY|unix.O_DIRECTORY|unix.O_NOFOLLOW|unix.O_CLOEXEC, 0)
	if err != nil {
		return pdInstallation{}, errPDInstallationUnavailable
	}
	defer unix.Close(root)
	return readPDInstallationAt(root, 0)
}
