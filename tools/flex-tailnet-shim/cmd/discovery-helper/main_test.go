package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"strings"
	"testing"
)

func frame(b []byte) []byte {
	out := binary.BigEndian.AppendUint16(nil, uint16(len(b)))
	return append(out, b...)
}

// relay broadcasts each announcement frame as it arrives, skips anything
// else, keeps going when a broadcast fails, and ends on a bad length.
func TestRelay(t *testing.T) {
	var in bytes.Buffer
	in.Write(frame([]byte("model=FLEX-6500 serial=1 ip=100.64.0.1")))
	in.Write(frame([]byte("not an announcement")))
	in.Write(frame([]byte("serial=2 ip=100.64.0.1"))) // its broadcast fails
	in.Write(frame([]byte("serial=3 ip=100.64.0.1")))
	in.Write([]byte{0x08, 0x01}) // 2049 bytes: too long
	var sent []string
	err := relay(&in, func(b []byte) error {
		sent = append(sent, string(b))
		if strings.HasPrefix(string(b), "serial=2") {
			return errors.New("no route")
		}
		return nil
	})
	if err == nil || !strings.Contains(err.Error(), "2049") {
		t.Fatalf("ended with %v, want a bad length", err)
	}
	want := "[model=FLEX-6500 serial=1 ip=100.64.0.1 serial=2 ip=100.64.0.1 serial=3 ip=100.64.0.1]"
	if got := fmt.Sprint(sent); got != want {
		t.Fatalf("broadcast %s\nwant %s", got, want)
	}
	// A zero length is malformed too; a dropped connection ends it.
	zero := append([]byte{0, 0}, frame([]byte("serial=1"))...)
	if err := relay(bytes.NewReader(zero), func([]byte) error { return nil }); err == nil || !strings.Contains(err.Error(), "length 0") {
		t.Fatalf("zero length: %v", err)
	}
	if err := relay(bytes.NewReader(frame([]byte("serial=1"))[:5]), func([]byte) error { return nil }); err != io.ErrUnexpectedEOF {
		t.Fatalf("truncated frame: %v", err)
	}
}
