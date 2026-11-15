//go:build linux

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"sync/atomic"
	"syscall"
	"testing"
	"time"

	"golang.org/x/sys/unix"
)

// Explicit native bench harness only. This observer is absent from the shipped
// binary. It exports owned-child diagnostics, and accepts no control commands.
func TestAetherPDManagedBench(t *testing.T) {
	mode := os.Getenv("AETHER_PD_LIVE_MANAGED_BENCH")
	if mode == "" {
		t.Skip("native managed bench harness required")
	}
	if mode != "receive" && mode != "rf" && mode != "calibration" && mode != "calibration-rf" && mode != "calibration-rf-cancel" {
		t.Fatal("invalid bench mode")
	}
	control, err := inheritedPDDescriptor("AETHER_PD_BENCH_OWNER_FD")
	if err != nil {
		t.Fatal(err)
	}
	var stat syscall.Stat_t
	if syscall.Fstat(control, &stat) != nil || stat.Mode&syscall.S_IFMT != syscall.S_IFIFO {
		t.Fatal("invalid bench lifetime")
	}
	syscall.CloseOnExec(control)
	defer syscall.Close(control)
	p, err := startInheritedAetherPD()
	if err != nil || p == nil {
		t.Fatal("managed launch failed", err)
	}
	t.Cleanup(func() {
		p.Stop()
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = p.Wait(ctx)
	})
	fmt.Printf("PD_CHILD_PID=%d\n", p.pid)
	fmt.Printf("PD_UNKEY_PID=%d\n", p.unkey.cmd.Process.Pid)
	listener, err := net.Listen("tcp4", "0.0.0.0:48996")
	if err != nil {
		t.Fatal("bench observer unavailable", err)
	}
	defer listener.Close()
	var calibrationVerified atomic.Bool
	var calibrationArmed atomic.Bool
	var calibrationCanceled atomic.Bool
	var calibrationKeyStart time.Time
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			func() {
				defer conn.Close()
				ctx, cancel := context.WithTimeout(context.Background(), time.Second)
				defer cancel()
				data, err := p.readNativeStatus(ctx)
				if err != nil {
					return
				}
				var fields map[string]json.RawMessage
				if json.Unmarshal(data, &fields) != nil {
					return
				}
				fields["bench_calibration_armed"], _ = json.Marshal(calibrationArmed.Load())
				fields["bench_calibration_canceled"], _ = json.Marshal(calibrationCanceled.Load())
				fields["bench_calibration_receive_pass"], _ = json.Marshal(calibrationVerified.Load())
				fields["bench_dsp_pid"], _ = json.Marshal(p.pid)
				fields["bench_unkey_pid"], _ = json.Marshal(p.unkey.cmd.Process.Pid)
				fields["bench_shim_pid"], _ = json.Marshal(os.Getpid())
				data, err = json.Marshal(fields)
				if err != nil {
					return
				}
				_ = conn.SetWriteDeadline(time.Now().Add(time.Second))
				_, _ = conn.Write(append(data, '\n'))
			}()
		}
	}()
	deadline := time.Now().Add(120 * time.Second)
	for time.Now().Before(deadline) {
		fds := []unix.PollFd{{Fd: int32(control)}}
		if _, err := unix.Poll(fds, 100); err != nil && err != unix.EINTR {
			t.Fatal(err)
		}
		if fds[0].Revents&(unix.POLLHUP|unix.POLLERR|unix.POLLNVAL) != 0 {
			n := &Node{pdProcess: p}
			n.cfg.TokenSHA256 = sha256Hex("managed-bench-stop")
			if reply := call(t, NewAPI(n), "POST", "/v1/aetherpd/stop", "192.168.50.20:4000", "managed-bench-stop", ""); reply.Code != http.StatusAccepted {
				t.Fatal(reply.Code)
			}
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			err := p.Wait(ctx)
			cancel()
			if err != nil {
				t.Fatal("owned DSP did not exit cleanly", err)
			}
			t.Log("managed bench ended through authenticated owned-session stop")
			return
		}
		if (mode == "calibration-rf" || mode == "calibration-rf-cancel") && !calibrationArmed.Load() {
			data, err := p.readNativeStatus(context.Background())
			var status struct {
				Active  int    `json:"active_slice"`
				Frames  uint64 `json:"local_iq_frames"`
				Keyed   bool   `json:"keyed"`
				Control bool   `json:"calibration_control"`
				Testing bool   `json:"test_active"`
			}
			if err == nil && json.Unmarshal(data, &status) == nil && status.Active >= 0 && status.Frames >= 500 {
				if status.Keyed || !status.Control || status.Testing {
					t.Fatal("RF calibration arm precondition", status)
				}
				n := &Node{pdProcess: p}
				n.cfg.TokenSHA256 = sha256Hex("managed-calibration-test")
				api := NewAPI(n)
				if got := call(t, api, "POST", "/v1/aetherpd/calibration/arm", "192.168.50.20:4000", "managed-calibration-test", ""); got.Code != http.StatusOK {
					t.Fatal(got.Code, got.Body.String())
				}
				if got := call(t, api, "GET", "/v1/aetherpd/status", "192.168.50.20:4000", "managed-calibration-test", ""); got.Code != http.StatusOK {
					t.Fatal(got.Code, got.Body.String())
				}
				calibrationArmed.Store(true)
				fmt.Println("PD_CALIBRATION_RF_ARMED=true")
			}
		}
		if mode == "calibration-rf-cancel" && calibrationArmed.Load() && !calibrationCanceled.Load() {
			data, err := p.readNativeStatus(context.Background())
			var status struct {
				Keyed   bool   `json:"keyed"`
				Testing bool   `json:"test_active"`
				Phase   string `json:"calibration_state"`
			}
			if err != nil || json.Unmarshal(data, &status) != nil {
				t.Fatal("cancel bench status unavailable", err)
			}
			if status.Keyed && calibrationKeyStart.IsZero() {
				calibrationKeyStart = time.Now()
			}
			if !calibrationKeyStart.IsZero() && time.Since(calibrationKeyStart) >= 3*time.Second {
				if !status.Keyed || !status.Testing || status.Phase != "running" {
					t.Fatal("cancel bench must still be keyed with source running", status)
				}
				n := &Node{pdProcess: p}
				n.cfg.TokenSHA256 = sha256Hex("managed-calibration-test")
				if got := call(t, NewAPI(n), "POST", "/v1/aetherpd/calibration/cancel", "192.168.50.20:4000", "managed-calibration-test", ""); got.Code != http.StatusOK {
					t.Fatal(got.Code, got.Body.String())
				}
				calibrationCanceled.Store(true)
				fmt.Println("PD_CALIBRATION_RF_CANCELED=true")
			}
		}
		if mode == "calibration" && !calibrationVerified.Load() {
			data, err := p.readNativeStatus(context.Background())
			var status struct {
				Active  int    `json:"active_slice"`
				Frames  uint64 `json:"local_iq_frames"`
				Keyed   bool   `json:"keyed"`
				Control bool   `json:"calibration_control"`
				Phase   string `json:"calibration_state"`
				Samples uint32 `json:"calibration_samples_left"`
				Testing bool   `json:"test_active"`
			}
			if err == nil && json.Unmarshal(data, &status) == nil && status.Active >= 0 && status.Frames >= 500 {
				if status.Keyed || !status.Control || status.Testing {
					t.Fatal("receive calibration precondition", status)
				}
				n := &Node{pdProcess: p}
				n.cfg.TokenSHA256 = sha256Hex("managed-calibration-test")
				api := NewAPI(n)
				if got := call(t, api, "POST", "/v1/aetherpd/calibration/arm", "192.168.50.20:4000", "wrong", ""); got.Code != http.StatusUnauthorized {
					t.Fatal(got.Code)
				}
				if got := call(t, api, "POST", "/v1/aetherpd/calibration/arm", "192.168.50.20:4000", "managed-calibration-test", ""); got.Code != http.StatusOK {
					t.Fatal(got.Code, got.Body.String())
				}
				for i := 0; i < 5; i++ {
					data, err = p.readNativeStatus(context.Background())
					if err != nil || json.Unmarshal(data, &status) != nil || status.Keyed || status.Testing || status.Phase != "armed" || status.Samples != 192000 {
						t.Fatal("armed receive source changed", status, err)
					}
					time.Sleep(100 * time.Millisecond)
				}
				if got := call(t, api, "POST", "/v1/aetherpd/calibration/cancel?seconds=99", "192.168.50.20:4000", "managed-calibration-test", ""); got.Code != http.StatusBadRequest {
					t.Fatal(got.Code)
				}
				if got := call(t, api, "POST", "/v1/aetherpd/calibration/cancel", "192.168.50.20:4000", "managed-calibration-test", ""); got.Code != http.StatusOK {
					t.Fatal(got.Code, got.Body.String())
				}
				data, err = p.readNativeStatus(context.Background())
				if err != nil || json.Unmarshal(data, &status) != nil || status.Keyed || status.Testing || status.Phase != "canceled" || status.Samples != 0 {
					t.Fatal("cancel failed", status, err)
				}
				fmt.Println("PD_CALIBRATION_RECEIVE_PASS=true")
				calibrationVerified.Store(true)
			}
		}
		select {
		case <-p.done:
			select {
			case <-p.unkey.done:
				fmt.Printf("PD_UNKEY_REAPED_OK=%t\n", p.unkey.err == nil)
			default:
				fmt.Println("PD_UNKEY_REAPED_OK=false")
			}
			t.Fatal("owned DSP exited before bench stop", p.err)
		default:
		}
	}
	t.Fatal("managed bench lifetime exceeded")
}
