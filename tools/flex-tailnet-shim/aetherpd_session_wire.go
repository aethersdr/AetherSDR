package main

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"sync"
	"time"
)

const pdSessionMaxBody = 8192
const pdSessionMaxPending = 8
const pdSessionSocket = "/run/aetherpd/start.sock"

var errPDSessionClosed = errors.New("AetherPD session channel unavailable")

type pdStartIdentity struct {
	GUIClientID string `json:"gui_client_id"`
	GUIHandle   uint32 `json:"gui_handle"`
	IQChannel   uint32 `json:"iq_channel,omitempty"`
	FeedbackPan uint32 `json:"feedback_pan,omitempty"`
}

func (id pdStartIdentity) frame() ([]byte, error) {
	frame := make([]byte, 48)
	if len(id.GUIClientID) != 36 || id.GUIHandle == 0 {
		return frame, errors.New("invalid GUI identity")
	}
	for i, c := range []byte(id.GUIClientID) {
		if i == 8 || i == 13 || i == 18 || i == 23 {
			if c != '-' {
				return frame, errors.New("invalid GUI UUID")
			}
			continue
		}
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f' || c >= 'A' && c <= 'F') {
			return frame, errors.New("invalid GUI UUID")
		}
	}
	if id.IQChannel != 0 || id.FeedbackPan != 0 {
		if id.IQChannel < 1 || id.IQChannel > 4 || id.FeedbackPan == 0 {
			return nil, errors.New("invalid IQ selection")
		}
		frame = make([]byte, 56)
		copy(frame[:8], "PDSTART2")
		binary.BigEndian.PutUint32(frame[48:52], id.IQChannel)
		binary.BigEndian.PutUint32(frame[52:56], id.FeedbackPan)
	} else {
		copy(frame[:8], "PDSTART1")
	}
	copy(frame[8:44], id.GUIClientID)
	binary.BigEndian.PutUint32(frame[44:], id.GUIHandle)
	return frame, nil
}

type pdSessionReply struct {
	code int
	body []byte
	err  error
}

type pdSessionChannel struct {
	// Immutable after publication: capture v2, tests v3, measurement v4,
	// correction v5, sampled source v6, configured tones v7, batch capture v8.
	batchSupported         bool
	toneSupported          bool
	captureSourceSupported bool
	correctionSupported    bool
	captureSupported       bool
	measurementSupported   bool
	testSupported          bool
	conn                   net.Conn
	writeMu                sync.Mutex
	mu                     sync.Mutex
	next                   uint32
	pending                map[uint32]chan pdSessionReply
	closed                 bool
}

func newPDSessionChannel(conn net.Conn) *pdSessionChannel {
	s := &pdSessionChannel{conn: conn, pending: make(map[uint32]chan pdSessionReply)}
	go s.readReplies()
	return s
}
func (s *pdSessionChannel) Close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return
	}
	s.closed = true
	s.conn.Close()
	for id, ch := range s.pending {
		ch <- pdSessionReply{err: errPDSessionClosed}
		delete(s.pending, id)
	}
}
func (s *pdSessionChannel) alive() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return !s.closed
}
func (s *pdSessionChannel) busy() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.pending) != 0
}
func pdCorrectionOpcode(op byte) bool { return op == '0' || op == '1' }
func pdSessionOpcode(op byte) bool {
	return op == 'S' || op == 'M' || op == 'A' || op == 'X' || op == 'T' || op == 'R' || pdCorrectionOpcode(op) || pdCaptureOpcode(op)
}
func (s *pdSessionChannel) Call(ctx context.Context, op byte) pdSessionReply {
	return s.call(ctx, op, nil)
}
func (s *pdSessionChannel) CallTest(ctx context.Context, op byte, id [16]byte, duration uint16) pdSessionReply {
	if !s.testSupported || !pdTestRequestValid(op, id, duration) {
		return pdSessionReply{err: errors.New("invalid or unsupported test operation")}
	}
	var extra [18]byte
	copy(extra[:16], id[:])
	binary.BigEndian.PutUint16(extra[16:], duration)
	return s.call(ctx, op, extra[:])
}
func (s *pdSessionChannel) CallTone(ctx context.Context, spacing uint32) pdSessionReply {
	if !s.toneSupported || !pdToneSpacingValid(spacing) {
		return pdSessionReply{err: errors.New("invalid or unsupported tone configuration")}
	}
	var extra [2]byte
	binary.BigEndian.PutUint16(extra[:], uint16(spacing))
	return s.call(ctx, 'Q', extra[:])
}
func (s *pdSessionChannel) CallBatch(ctx context.Context, op byte, value uint16) pdSessionReply {
	if !s.batchSupported || !pdCaptureBatchRequestValid(op, value) {
		return pdSessionReply{err: errors.New("invalid or unsupported batch capture")}
	}
	var extra [2]byte
	binary.BigEndian.PutUint16(extra[:], value)
	return s.call(ctx, op, extra[:])
}
func (s *pdSessionChannel) call(ctx context.Context, op byte, extra []byte) pdSessionReply {
	validExtra := (s.testSupported && pdTestOpcode(op) && len(extra) == 18) ||
		(s.toneSupported && op == 'Q' && len(extra) == 2 && pdToneSpacingValid(uint32(binary.BigEndian.Uint16(extra)))) ||
		(s.batchSupported && len(extra) == 2 && pdCaptureBatchRequestValid(op, binary.BigEndian.Uint16(extra)))
	if (extra == nil && !pdSessionOpcode(op)) || (extra != nil && !validExtra) || (pdCaptureOpcode(op) && !s.captureSupported) || (op == pdCaptureBeginWithSource && !s.captureSourceSupported) || (op == pdCaptureBeginWithTones && !s.toneSupported) || (op == 'M' && !s.measurementSupported) || (pdCorrectionOpcode(op) && !s.correctionSupported) {
		return pdSessionReply{err: errors.New("invalid session operation")}
	}
	if err := ctx.Err(); err != nil {
		return pdSessionReply{err: err}
	}
	s.writeMu.Lock()
	if err := ctx.Err(); err != nil {
		s.writeMu.Unlock()
		return pdSessionReply{err: err}
	}
	s.mu.Lock()
	if s.closed || s.next == ^uint32(0) || len(s.pending) >= pdSessionMaxPending {
		s.mu.Unlock()
		s.writeMu.Unlock()
		return pdSessionReply{err: errPDSessionClosed}
	}
	s.next++
	id := s.next
	ch := make(chan pdSessionReply, 1)
	s.pending[id] = ch
	s.mu.Unlock()
	frame := make([]byte, 16+len(extra))
	copy(frame[:8], "PDCMD001")
	if extra != nil {
		copy(frame[:8], "PDCMD002")
		if op == 'Q' {
			copy(frame[:8], "PDCMD003")
		}
		if len(extra) == 2 && pdCaptureBatchOpcode(op) {
			copy(frame[:8], "PDCMD004")
		}
		copy(frame[16:], extra)
	}
	binary.BigEndian.PutUint32(frame[8:12], id)
	frame[12] = op
	// An abandoned calibration/control request retires its owned session so a
	// delayed arm cannot outlive intent. Status and capture only observe: retain
	// their bounded pending slots until readReplies drains late responses. A test
	// stop is already bound to its run ID; abandoning it must not close a
	// successor's session before that ID is checked.
	cancelClose := func() bool { return true }
	if op != 'S' && !pdCaptureOpcode(op) && !pdCaptureBatchOpcode(op) && op != pdTestStatus && op != pdTestStop {
		cancelClose = context.AfterFunc(ctx, s.Close)
	}
	defer cancelClose()
	err := s.conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
	if err == nil {
		err = pdWriteAll(s.conn, frame[:])
	}
	s.writeMu.Unlock()
	if err != nil {
		s.Close()
		return pdSessionReply{err: err}
	}
	select {
	case reply := <-ch:
		return reply
	case <-ctx.Done():
		if op != 'S' && !pdCaptureOpcode(op) && !pdCaptureBatchOpcode(op) && op != pdTestStatus && op != pdTestStop {
			s.Close()
		}
		return pdSessionReply{err: ctx.Err()}
	}
}
func pdWriteAll(w io.Writer, data []byte) error {
	for len(data) > 0 {
		n, err := w.Write(data)
		if err != nil {
			return err
		}
		if n <= 0 {
			return io.ErrShortWrite
		}
		data = data[n:]
	}
	return nil
}
func (s *pdSessionChannel) readReplies() {
	defer s.Close()
	for {
		var header [16]byte
		if _, err := io.ReadFull(s.conn, header[:]); err != nil {
			return
		}
		if string(header[:8]) != "PDREP001" {
			return
		}
		id := binary.BigEndian.Uint32(header[8:12])
		code := int(binary.BigEndian.Uint16(header[12:14]))
		size := int(binary.BigEndian.Uint16(header[14:]))
		if id == 0 || code < 200 || code > 599 || size == 0 || size > pdSessionMaxBody {
			return
		}
		body := make([]byte, size)
		if _, err := io.ReadFull(s.conn, body); err != nil {
			return
		}
		s.mu.Lock()
		ch, ok := s.pending[id]
		if ok {
			delete(s.pending, id)
			ch <- pdSessionReply{code: code, body: body}
		}
		s.mu.Unlock()
		if !ok {
			return
		}
	}
}
