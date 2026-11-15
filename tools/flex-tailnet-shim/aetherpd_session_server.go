package main

import (
	"context"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
)

// This writer bounds even a future handler's response before a private channel
// can allocate or transmit it. Headers never become a second command surface.
type pdSessionResponse struct {
	header   http.Header
	code     int
	body     []byte
	overflow bool
}

func (w *pdSessionResponse) Header() http.Header { return w.header }
func (w *pdSessionResponse) WriteHeader(code int) {
	if w.code == 0 {
		w.code = code
	}
}
func (w *pdSessionResponse) Write(b []byte) (int, error) {
	if len(w.body)+len(b) > pdSessionMaxBody {
		w.overflow = true
		return 0, errors.New("session response too large")
	}
	if w.code == 0 {
		w.code = 200
	}
	w.body = append(w.body, b...)
	return len(b), nil
}

type pdSessionOperation func(context.Context, byte) pdSessionReply
type pdSessionTestOperation func(context.Context, byte, [16]byte, uint16) pdSessionReply

func servePDSession(conn net.Conn, operation pdSessionOperation, stop func(), testOperations ...pdSessionTestOperation) error {
	if len(testOperations) > 1 {
		conn.Close()
		stop()
		return errors.New("invalid test operation handler")
	}
	var testOperation pdSessionTestOperation
	if len(testOperations) == 1 {
		testOperation = testOperations[0]
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	defer conn.Close()
	defer stop()
	var writers sync.Mutex
	var tasks sync.WaitGroup
	// Cancel before waiting, including a pending arm. The caller additionally
	// retires its DSP lifetime before any worker's shutdown wait.
	defer func() { cancel(); conn.Close(); stop(); tasks.Wait() }()
	if err := conn.SetWriteDeadline(time.Now().Add(2 * time.Second)); err != nil {
		return err
	}
	hello := "PDTALK02"
	if testOperation != nil {
		hello = "PDTALK03"
	}
	if err := pdWriteAll(conn, []byte(hello)); err != nil {
		return err
	}
	slots := make(chan struct{}, pdSessionMaxPending)
	var last uint32
	for {
		if err := conn.SetReadDeadline(time.Now().Add(15 * time.Second)); err != nil {
			return err
		}
		var frame [16]byte
		if _, err := io.ReadFull(conn, frame[:]); err != nil {
			return err
		}
		id := binary.BigEndian.Uint32(frame[8:12])
		op := frame[12]
		isTest := string(frame[:8]) == "PDCMD002" && testOperation != nil && pdTestOpcode(op)
		legacy := string(frame[:8]) == "PDCMD001" && pdSessionOpcode(op)
		if (!isTest && !legacy) || id <= last || frame[13] != 0 || frame[14] != 0 || frame[15] != 0 {
			return errors.New("invalid session request")
		}
		var testID [16]byte
		var duration uint16
		if isTest {
			var extra [18]byte
			if _, err := io.ReadFull(conn, extra[:]); err != nil {
				return err
			}
			copy(testID[:], extra[:16])
			duration = binary.BigEndian.Uint16(extra[16:])
			if !pdTestRequestValid(op, testID, duration) {
				return errors.New("invalid test request")
			}
		}
		last = id
		select {
		case slots <- struct{}{}:
		default:
			return errors.New("too many session requests")
		}
		tasks.Add(1)
		go func() {
			defer tasks.Done()
			defer func() { <-slots }()
			callCtx, finish := context.WithTimeout(ctx, 8*time.Second)
			defer finish()
			var reply pdSessionReply
			if isTest {
				reply = testOperation(callCtx, op, testID, duration)
			} else {
				reply = operation(callCtx, op)
			}
			if reply.err != nil || reply.code < 200 || reply.code > 599 || len(reply.body) == 0 || len(reply.body) > pdSessionMaxBody {
				cancel()
				conn.Close()
				return
			}
			var header [16]byte
			copy(header[:8], "PDREP001")
			binary.BigEndian.PutUint32(header[8:12], id)
			binary.BigEndian.PutUint16(header[12:14], uint16(reply.code))
			binary.BigEndian.PutUint16(header[14:], uint16(len(reply.body)))
			writers.Lock()
			defer writers.Unlock()
			if err := conn.SetWriteDeadline(time.Now().Add(2 * time.Second)); err != nil {
				cancel()
				conn.Close()
				return
			}
			if pdWriteAll(conn, header[:]) != nil || pdWriteAll(conn, reply.body) != nil {
				cancel()
				conn.Close()
			}
		}()
	}
}

func pdSessionOperations(n *Node) pdSessionOperation {
	const token = "private-inherited-session-channel"
	n.cfg.TokenSHA256 = sha256Hex(token)
	handlers := map[byte]http.HandlerFunc{
		'S': aetherPDStatusHandler(n, n.pdProcess.queryStatus),
		'A': aetherPDCalibrationHandler(n, 'A'),
		'X': aetherPDCalibrationHandler(n, 'X'),
		'R': aetherPDReceiveRestoreHandler(n),
		'T': func(w http.ResponseWriter, r *http.Request) {
			n.pdProcess.Stop()
			writeJSON(w, http.StatusAccepted, map[string]string{"state": "stop_requested"})
		},
	}
	return func(ctx context.Context, op byte) pdSessionReply {
		if pdCaptureOpcode(op) {
			return n.pdProcess.captureOperation(ctx, op)
		}
		handler, ok := handlers[op]
		if !ok {
			return pdSessionReply{err: errors.New("unknown session operation")}
		}
		request, _ := http.NewRequestWithContext(ctx, http.MethodPost, "http://private/", strings.NewReader(""))
		request.Header.Set("Authorization", "Bearer "+token)
		response := &pdSessionResponse{header: make(http.Header)}
		handler(response, request)
		if response.overflow {
			return pdSessionReply{err: errors.New("session response too large")}
		}
		return pdSessionReply{code: response.code, body: response.body}
	}
}
