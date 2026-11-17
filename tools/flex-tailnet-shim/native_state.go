package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"iter"
	"maps"
	"os"
	"path/filepath"
	"sync"

	"tailscale.com/ipn"
	"tailscale.com/ipn/store"
)

// The authenticated native installer can transfer at most 64 KiB per identity
// file (pd_state_transfer.c). Include JSON/base64 overhead in this limit.
const nativeStateFileLimit = 64 * 1024

var errNativeStateLimit = errors.New("native identity file exceeds the 64 KiB limit")
var errNativeCommitUncertain = errors.New("native identity replacement durability or selection is uncertain")
var errNativeStateUnavailable = errors.New("native identity is unavailable; explicit recovery required")

// Per-write operations allow independent file/directory sync failures in tests
// without global hooks or another persistent file.
type nativeStateCommitOps struct {
	sync   func(*os.File) error
	rename func(*os.File, string, string) error
}

func nativeStateCommitOperations() nativeStateCommitOps {
	return nativeStateCommitOps{sync: (*os.File).Sync, rename: renameNativeStateFileAt}
}

func readNativeStateFile(path string) ([]byte, error) {
	f, err := openNativeStateFile(path, false)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() {
		return nil, errors.New("native identity is not a regular file")
	}
	if info.Size() > nativeStateFileLimit {
		return nil, errNativeStateLimit
	}
	data, err := io.ReadAll(io.LimitReader(f, nativeStateFileLimit+1))
	if err != nil {
		return nil, err
	}
	if len(data) > nativeStateFileLimit {
		return nil, errNativeStateLimit
	}
	return data, nil
}

// A fixed, private temporary name bounds crash remnants to one file per
// identity. Callers serialize writes. Sync the parent before mutation and again
// after file sync and rename. An error after rename is an uncertain commit,
// not proof that the previous file remains selected.
func writeNativeStateFile(path string, data []byte) error {
	return writeNativeStateFileWithOps(path, data, nativeStateCommitOperations())
}

func writeNativeStateFileWithOps(path string, data []byte, ops nativeStateCommitOps) (err error) {
	if len(data) > nativeStateFileLimit {
		return errNativeStateLimit
	}
	parent, err := openNativeStateDirectory(filepath.Dir(path))
	if err != nil {
		return err
	}
	defer parent.Close()
	if err = ops.sync(parent); err != nil {
		return err
	}
	parentInfo, err := parent.Stat()
	if err != nil {
		return err
	}
	selectedParent := func() error {
		current, statErr := os.Lstat(filepath.Dir(path))
		if statErr != nil {
			return statErr
		}
		if !os.SameFile(parentInfo, current) {
			return errors.New("native identity parent changed")
		}
		return nil
	}
	if err = selectedParent(); err != nil {
		return err
	}
	name := filepath.Base(path)
	temporary := name + ".tmp"
	f, err := openNativeStateFileAt(parent, temporary)
	if err != nil {
		return err
	}
	defer func() {
		f.Close()
		if err != nil {
			removeNativeStateFileAt(parent, temporary)
		}
	}()
	if err = f.Chmod(0600); err != nil {
		return err
	}
	if err = f.Truncate(0); err != nil {
		return err
	}
	if _, err = f.Write(data); err != nil {
		return err
	}
	if err = ops.sync(f); err != nil {
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	if err = ops.rename(parent, temporary, name); err != nil {
		return err
	}
	if err = ops.sync(parent); err != nil {
		return fmt.Errorf("%w: %w", errNativeCommitUncertain, err)
	}
	if err = selectedParent(); err != nil {
		return fmt.Errorf("%w: %w", errNativeCommitUncertain, err)
	}
	return nil
}

// nativeStateStore retains Tailscale's map-of-base64-values file format.
// Publish a new cache only after its bounded file replacement succeeds.
// Reads and exports return copies so callers cannot bypass write admission.
// An uncertain commit blocks live reads; export retains the last committed
// snapshot. A successful full replacement repairs the store before reuse.
type nativeStateStore struct {
	mu              sync.Mutex
	path            string
	cache           map[ipn.StateKey][]byte
	commitUncertain bool
	commitOps       *nativeStateCommitOps // nil uses real filesystem operations
}

var _ store.ExportableStore = (*nativeStateStore)(nil)

// Tailscale includes its store in key-parse errors; never format the cache.
func (*nativeStateStore) String() string     { return "NativeStateStore" }
func (s *nativeStateStore) GoString() string { return s.String() }

func newNativeStateStore(path string) (*nativeStateStore, error) {
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return nil, err
	}
	data, err := readNativeStateFile(path)
	st := &nativeStateStore{path: path, cache: make(map[ipn.StateKey][]byte)}
	if errors.Is(err, os.ErrNotExist) || (err == nil && len(data) == 0) {
		if err = writeNativeStateFile(path, []byte("{}")); err != nil {
			return nil, err
		}
		return st, nil
	}
	if err != nil {
		return nil, err
	}
	if err = json.Unmarshal(data, &st.cache); err != nil {
		return nil, fmt.Errorf("invalid native identity JSON: %w", err)
	}
	if st.cache == nil {
		return nil, errors.New("native identity must be a JSON object")
	}
	return st, nil
}

func (s *nativeStateStore) ReadState(key ipn.StateKey) ([]byte, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.commitUncertain {
		return nil, errNativeCommitUncertain
	}
	data, ok := s.cache[key]
	if !ok {
		return nil, ipn.ErrStateNotExist
	}
	return bytes.Clone(data), nil
}

func (s *nativeStateStore) WriteState(key ipn.StateKey, data []byte) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(key) > nativeStateFileLimit || len(data) > nativeStateFileLimit {
		return errNativeStateLimit
	}
	previous, present := s.cache[key]
	if !s.commitUncertain && (data == nil && !present || data != nil && present && bytes.Equal(previous, data)) {
		return nil
	}
	next := maps.Clone(s.cache)
	if data == nil {
		delete(next, key)
	} else {
		next[key] = bytes.Clone(data)
	}
	encoded, err := json.MarshalIndent(next, "", "  ")
	if err != nil {
		return err
	}
	ops := nativeStateCommitOperations()
	if s.commitOps != nil {
		ops = *s.commitOps
	}
	if err = writeNativeStateFileWithOps(s.path, encoded, ops); err != nil {
		if errors.Is(err, errNativeCommitUncertain) {
			s.commitUncertain = true
		}
		return err
	}
	s.cache = next
	s.commitUncertain = false
	return nil
}

func (s *nativeStateStore) All() iter.Seq2[ipn.StateKey, []byte] {
	return func(yield func(ipn.StateKey, []byte) bool) {
		s.mu.Lock()
		snapshot := make(map[ipn.StateKey][]byte, len(s.cache))
		for key, value := range s.cache {
			snapshot[key] = bytes.Clone(value)
		}
		s.mu.Unlock()
		for key, value := range snapshot {
			if !yield(key, value) {
				return
			}
		}
	}
}
