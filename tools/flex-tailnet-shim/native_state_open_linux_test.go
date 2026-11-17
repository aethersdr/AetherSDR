package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"

	"golang.org/x/sys/unix"
)

func TestNativeStateRefusesTemporaryLinksAndSpecialFiles(t *testing.T) {
	for _, kind := range []string{"symlink", "hardlink", "fifo"} {
		t.Run(kind, func(t *testing.T) {
			n, st := nativeTestStore(t)
			path := filepath.Join(n.tsnetDir(), "tailscaled.state")
			if err := st.WriteState("key", []byte("previous")); err != nil {
				t.Fatal(err)
			}
			before, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			switch kind {
			case "symlink":
				err = os.Symlink(path, path+".tmp")
			case "hardlink":
				err = os.Link(path, path+".tmp")
			case "fifo":
				err = unix.Mkfifo(path+".tmp", 0600)
			}
			if err != nil {
				t.Fatal(err)
			}
			if err := st.WriteState("key", []byte("replacement")); err == nil {
				t.Fatal("unsafe temporary identity accepted")
			}
			after, err := os.ReadFile(path)
			if err != nil || !bytes.Equal(before, after) {
				t.Fatal("rejected temporary file changed the live identity")
			}
			if value, err := st.ReadState("key"); err != nil || string(value) != "previous" {
				t.Fatal("rejected temporary file changed cached identity")
			}
		})
	}
}

func TestNativeStateRefusesSpecialFileReads(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "state")
	if err := unix.Mkfifo(path, 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := readNativeStateFile(path); err == nil {
		t.Fatal("FIFO accepted as native identity")
	}
	if err := os.Remove(path); err != nil {
		t.Fatal(err)
	}
	target := filepath.Join(dir, "target")
	if err := os.WriteFile(target, []byte("{}"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(target, path); err != nil {
		t.Fatal(err)
	}
	if _, err := readNativeStateFile(path); err == nil {
		t.Fatal("symbolic link accepted as native identity")
	}
}
