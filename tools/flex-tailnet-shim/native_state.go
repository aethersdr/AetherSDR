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
// identity. Callers serialize writes. File sync precedes atomic replacement;
// parent-directory durability
// under abrupt power loss is a separate installation qualification gate.
func writeNativeStateFile(path string, data []byte) (err error) {
	if len(data) > nativeStateFileLimit {
		return errNativeStateLimit
	}
	temporary := path + ".tmp"
	f, err := openNativeStateFile(temporary, true)
	if err != nil {
		return err
	}
	defer func() {
		f.Close()
		if err != nil {
			os.Remove(temporary)
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
	if err = f.Sync(); err != nil {
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	return os.Rename(temporary, path)
}

// nativeStateStore retains Tailscale's map-of-base64-values file format.
// Publish a new cache only after its bounded file replacement succeeds.
// Reads and exports return copies so callers cannot bypass write admission.
type nativeStateStore struct {
	mu    sync.Mutex
	path  string
	cache map[ipn.StateKey][]byte
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
	if data == nil && !present || data != nil && present && bytes.Equal(previous, data) {
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
	if err = writeNativeStateFile(s.path, encoded); err != nil {
		return err
	}
	s.cache = next
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
