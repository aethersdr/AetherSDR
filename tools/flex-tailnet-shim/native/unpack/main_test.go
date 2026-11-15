//go:build unix

package main

import (
	"bytes"
	"compress/gzip"
	"os"
	"path/filepath"
	"testing"
)

func gz(t *testing.T, b []byte) []byte {
	var buf bytes.Buffer
	w := gzip.NewWriter(&buf)
	w.Write(b)
	w.Close()
	return buf.Bytes()
}

func TestUnpackWritesAnExecutable(t *testing.T) {
	dir := t.TempDir()
	src, dst := filepath.Join(dir, "s.gz"), filepath.Join(dir, "run", "shim")
	want := bytes.Repeat([]byte("shim"), 1000)
	os.WriteFile(src, gz(t, want), 0o644)
	if err := unpack(src, dst); err != nil {
		t.Fatal(err)
	}
	got, _ := os.ReadFile(dst)
	fi, _ := os.Stat(dst)
	if !bytes.Equal(got, want) || fi.Mode().Perm() != 0o755 {
		t.Fatalf("got %d bytes mode %v", len(got), fi.Mode())
	}
}

func TestATruncatedArchiveLeavesTheOldBinary(t *testing.T) {
	dir := t.TempDir()
	src, dst := filepath.Join(dir, "s.gz"), filepath.Join(dir, "shim")
	whole := gz(t, bytes.Repeat([]byte("new"), 5000))
	os.WriteFile(src, whole[:len(whole)-10], 0o644)
	os.WriteFile(dst, []byte("old"), 0o755)
	if err := unpack(src, dst); err == nil {
		t.Fatal("a truncated archive unpacked")
	}
	if got, _ := os.ReadFile(dst); string(got) != "old" {
		t.Fatalf("dst replaced: %q", got)
	}
	if ents, _ := os.ReadDir(dir); len(ents) != 2 {
		t.Fatalf("temp file left behind: %v", ents)
	}
}

// The run directory is shared RAM: someone else's directory, or one others
// can write, could swap the binary before the launcher runs it.
func TestUnpackRefusesADirectoryOthersControl(t *testing.T) {
	dir := t.TempDir()
	src := filepath.Join(dir, "s.gz")
	os.WriteFile(src, gz(t, []byte("shim")), 0o644)

	open := filepath.Join(dir, "open")
	os.Mkdir(open, 0o700)
	os.Chmod(open, 0o777)
	if err := unpack(src, filepath.Join(open, "shim")); err == nil {
		t.Fatal("unpacked into a world-writable directory")
	}

	real := filepath.Join(dir, "real")
	os.Mkdir(real, 0o700)
	link := filepath.Join(dir, "link")
	if err := os.Symlink(real, link); err != nil {
		t.Fatal(err)
	}
	if err := unpack(src, filepath.Join(link, "shim")); err == nil {
		t.Fatal("unpacked through a symlinked directory")
	}

	if err := unpack(src, filepath.Join(real, "shim")); err != nil {
		t.Fatalf("a private directory was refused: %v", err)
	}
}
