//go:build linux

package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"golang.org/x/sys/unix"
)

func TestPDInstallationProtectedReader(t *testing.T) {
	root, err := os.MkdirTemp("/dev/shm", "pd-installation-reader-")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.RemoveAll(root) })
	dir := filepath.Join(root, "run", "aetherpd")
	if err := os.MkdirAll(dir, 0755); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "installation")
	write := func(data []byte) {
		t.Helper()
		temp := filepath.Join(dir, "new")
		if err := os.WriteFile(temp, data, 0444); err != nil {
			t.Fatal(err)
		}
		if err := os.Rename(temp, path); err != nil {
			t.Fatal(err)
		}
	}
	fd, err := unix.Open(root, unix.O_RDONLY|unix.O_DIRECTORY|unix.O_CLOEXEC, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer unix.Close(fd)
	read := func() (pdInstallation, error) { return readPDInstallationAt(fd, uint32(os.Getuid())) }
	refused := func() {
		t.Helper()
		if _, err := read(); err == nil {
			t.Fatal("unprotected record accepted")
		}
	}
	refused()
	write(installationRecord(4))
	for _, phase := range []uint32{4, 7} {
		write(installationRecord(phase))
		status, err := read()
		if err != nil || status.Committed != (phase == 4) {
			t.Fatalf("replacement stale or unreadable: %+v %v", status, err)
		}
	}
	if _, err := readPDInstallationAt(fd, uint32(os.Getuid()+1)); err == nil {
		t.Fatal("wrong owner accepted")
	}
	for _, parent := range []string{root, filepath.Dir(dir), dir} {
		if err := os.Chmod(parent, 0777); err != nil {
			t.Fatal(err)
		}
		refused()
		if err := os.Chmod(parent, 0755); err != nil {
			t.Fatal(err)
		}
	}
	for _, mode := range []os.FileMode{0600, 0644, 0666} {
		if err := os.Chmod(path, mode); err != nil {
			t.Fatal(err)
		}
		refused()
	}
	os.Chmod(path, 0444)
	alias := filepath.Join(root, "alias")
	if err := os.Link(path, alias); err != nil {
		t.Fatal(err)
	}
	refused()
	os.Remove(alias)
	if err := os.Rename(path, alias); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(alias, path); err != nil {
		t.Fatal(err)
	}
	refused()
	os.Remove(path)
	if err := unix.Mkfifo(path, 0444); err != nil {
		t.Fatal(err)
	}
	refused()
	os.Remove(path)
	write(installationRecord(4)[:203])
	refused()
	write(installationRecord(4))
	moved := dir + "-old"
	if err := os.Rename(dir, moved); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(moved, dir); err != nil {
		t.Fatal(err)
	}
	refused()
}

func TestPDInstallationReaderRejectsDisk(t *testing.T) {
	root := t.TempDir()
	var fs unix.Statfs_t
	if err := unix.Statfs(root, &fs); err != nil {
		t.Fatal(err)
	}
	if fs.Type == unix.TMPFS_MAGIC {
		t.Skip("temporary test directory is on tmpfs")
	}
	dir := filepath.Join(root, "run", "aetherpd")
	if err := os.MkdirAll(dir, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "installation"), installationRecord(4), 0444); err != nil {
		t.Fatal(err)
	}
	fd, err := unix.Open(root, unix.O_RDONLY|unix.O_DIRECTORY|unix.O_CLOEXEC, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer unix.Close(fd)
	if _, err := readPDInstallationAt(fd, uint32(os.Getuid())); err == nil {
		t.Fatal("disk record accepted")
	}
}

// The native harness supplies its protected tmpfs root for format interoperability.
func TestPDInstallationNativePublisher(t *testing.T) {
	root := os.Getenv("AETHERPD_INSTALLATION_TEST_ROOT")
	if root == "" {
		t.Skip("native publisher fixture not supplied")
	}
	fd, err := unix.Open(root, unix.O_RDONLY|unix.O_DIRECTORY|unix.O_CLOEXEC, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer unix.Close(fd)
	st, err := readPDInstallationAt(fd, uint32(os.Getuid()))
	if err != nil || st.PlanDigest != os.Getenv("AETHERPD_INSTALLATION_TEST_PLAN") || st.Profile != 1 || st.Serial != "fixture-8600" || st.Firmware != "4.3.16.42034" {
		t.Fatalf("native publication mismatch: %+v %v", st, err)
	}
	if directory := os.Getenv("AETHERPD_INSTALLATION_EXPORT"); directory != "" {
		data, err := json.Marshal(st)
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(directory, st.State+".json"), append(data, '\n'), 0644); err != nil {
			t.Fatal(err)
		}
	}
	wantCommitted := os.Getenv("AETHERPD_INSTALLATION_TEST_PHASE") == "4"
	wantDigest, wantKey := "", ""
	if wantCommitted {
		wantDigest = strings.Repeat("09", 32)
		wantKey = strings.Repeat("03", 32)
	} else {
		wantDigest = strings.Repeat("01", 32)
		wantKey = strings.Repeat("02", 32)
	}
	if st.Committed != wantCommitted || st.SelectedDigest != wantDigest || st.SelectedKey != wantKey {
		t.Fatalf("native selection mismatch: %+v", st)
	}
}
