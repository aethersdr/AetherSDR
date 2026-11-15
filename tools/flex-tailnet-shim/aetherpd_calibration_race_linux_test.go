//go:build linux

package main

import (
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestAetherPDCalibrationCancelSupersedesPendingArmSnapshot(t *testing.T) {
	for _, opcode := range []byte{'A', 'M'} {
		t.Run(string(opcode), func(t *testing.T) {
			n := newTestNode(t, "secret")
			p, peer := calibrationPair(t)
			n.pdProcess = p
			if opcode == 'M' {
				measurementStatusFixture(t, p, pdMeasurementNativeStatus)
			}
			journal, view, _ := receiveJournalFixture()
			p.receiveJournal = journal
			var armed atomic.Bool
			peerDone := make(chan struct{})
			go func() {
				defer close(peerDone)
				for {
					request := make([]byte, 16)
					size, err := peer.Read(request)
					if err != nil {
						return
					}
					if size != 9 {
						return
					}
					armed.Store(request[0] == opcode)
					if _, err := peer.Write(append(append([]byte{}, request[1:9]...), 0)); err != nil {
						return
					}
				}
			}()
			t.Cleanup(func() { peer.Close(); <-peerDone })
			entered, release := make(chan struct{}), make(chan struct{})
			var releaseOnce sync.Once
			t.Cleanup(func() { releaseOnce.Do(func() { close(release) }) })
			observe := func(context.Context, pdReceiveOriginal) (pdReceiveView, error) {
				close(entered)
				<-release
				view.observedAt = time.Now()
				return view, nil
			}
			api := http.NewServeMux()
			api.HandleFunc("POST /arm", aetherPDCalibrationHandlerWithObserver(n, opcode, observe))
			api.HandleFunc("POST /cancel", aetherPDCalibrationHandlerWithObserver(n, 'X', observe))
			armDone := make(chan int, 1)
			go func() { armDone <- call(t, api, "POST", "/arm", "192.168.50.20:4000", "secret", "").Code }()
			select {
			case <-entered:
			case <-time.After(time.Second):
				t.Fatal("arm did not reach snapshot acquisition")
			}
			if got := call(t, api, "POST", "/cancel", "192.168.50.20:4000", "secret", ""); got.Code != http.StatusOK {
				t.Fatal(got.Code, got.Body.String())
			}
			releaseOnce.Do(func() { close(release) })
			select {
			case code := <-armDone:
				if code == http.StatusOK || armed.Load() {
					t.Fatalf("cancel completed before snapshot, but old arm still reached source: HTTP%d armed=%v", code, armed.Load())
				}
			case <-time.After(2 * time.Second):
				t.Fatal("pending arm did not finish")
			}

		})
	}
}

func TestAetherPDCalibrationUnconfirmedSafetyCancelEndsOwnedLifetime(t *testing.T) {
	p, peer := calibrationPair(t)
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Close()
	defer writer.Close()
	p.owner = writer
	seen := make(chan byte, 1)
	go func() {
		request := make([]byte, 16)
		size, err := peer.Read(request)
		if err == nil && size == 9 {
			seen <- request[0]
		}
	}()
	cancelFailedAetherPDArm(p)
	select {
	case op := <-seen:
		if op != 'X' {
			t.Fatal("safety cleanup did not send cancel")
		}
	default:
		t.Fatal("safety cancel missing")
	}
	if err := reader.SetReadDeadline(time.Now().Add(time.Second)); err != nil {
		t.Fatal(err)
	}
	if _, err := reader.Read(make([]byte, 1)); err != io.EOF {
		t.Fatal("unconfirmed cancellation left owned DSP lifetime open", err)
	}
}

func TestAetherPDCalibrationCanceledReplyWaitCancelsNativeSource(t *testing.T) {
	for _, opcode := range []byte{'A', 'M'} {
		t.Run(string(opcode), func(t *testing.T) {
			n := newTestNode(t, "secret")
			p, peer := calibrationPair(t)
			n.pdProcess = p
			if opcode == 'M' {
				measurementStatusFixture(t, p, pdMeasurementNativeStatus)
			}
			var armed atomic.Bool
			requestSeen := make(chan struct{})
			peerDone := make(chan struct{})
			go func() {
				defer close(peerDone)
				request := make([]byte, 16)
				size, err := peer.Read(request)
				if err != nil || size != 9 || request[0] != opcode {
					return
				}
				armed.Store(true)
				close(requestSeen)
				// The source accepted A, but the reply is lost. X must use a fresh
				// bounded context even after the HTTP request has been canceled.
				size, err = peer.Read(request)
				if err != nil || size != 9 || request[0] != 'X' {
					return
				}
				armed.Store(false)
				_, _ = peer.Write(append(append([]byte{}, request[1:9]...), 0))
			}()
			t.Cleanup(func() { peer.Close(); <-peerDone })
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			path := "/v1/aetherpd/calibration/arm"
			if opcode == 'M' {
				path = "/v1/aetherpd/measurement/arm"
			}
			req := httptest.NewRequest("POST", path, nil).WithContext(ctx)
			req.RemoteAddr = "192.168.50.20:4000"
			req.Header.Set("Authorization", "Bearer secret")
			response := httptest.NewRecorder()
			handlerDone := make(chan struct{})
			go func() { NewAPI(n).ServeHTTP(response, req); close(handlerDone) }()
			select {
			case <-requestSeen:
			case <-time.After(time.Second):
				t.Fatal("native arm request missing")
			}
			cancel()
			select {
			case <-handlerDone:
				if response.Code != http.StatusServiceUnavailable || armed.Load() {
					t.Fatalf("canceled caller left source armed: HTTP%d armed=%v", response.Code, armed.Load())
				}
			case <-time.After(2 * time.Second):
				t.Fatal("failed arm cancellation did not finish")
			}

		})
	}
}
