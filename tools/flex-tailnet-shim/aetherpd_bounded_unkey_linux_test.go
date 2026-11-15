//go:build linux

package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"
)

func TestPDBoundedReadiness(t *testing.T) {
	now, err := pdMonotonicMS()
	if err != nil {
		t.Fatal(err)
	}
	valid := fmt.Sprintf(`{"event":"bounded_ready","cleanup_due_ms":"%d","keys_transmitter":false}`, now+10000)
	if due, err := pdUnkeyReadiness([]byte(valid), 15000); err != nil || due != now+10000 {
		t.Fatal("valid bounded readiness refused", due, err)
	}
	for _, line := range []string{
		valid + `{}`, strings.Replace(valid, `false`, `null`, 1), strings.Replace(valid, `false`, `true`, 1),
		strings.Replace(valid, `"event"`, `"Event"`, 1),
		strings.Replace(valid, `"event":`, `"event":"ready","event":`, 1),
		strings.Replace(valid, `"keys_transmitter":false`, `"keys_transmitter":true,"keys_transmitter":false`, 1),
		strings.Replace(valid, `"cleanup_due_ms":`, `"cleanup_due_ms":"1","cleanup_due_ms":`, 1),
		strings.Replace(valid, `"bounded_ready"`, `"ready"`, 1),
		strings.Replace(valid, `"cleanup_due_ms"`, `"extra"`, 1),
		`{"event":"bounded_ready","keys_transmitter":false}`, `null`, `[]`,
	} {
		if _, err := pdUnkeyReadiness([]byte(line), 15000); err == nil {
			t.Fatal("invalid readiness accepted", line)
		}
	}
	for _, due := range []string{"0", "1", "-1", "+100", "00100", "9223372036854775808", fmt.Sprint(now - 1), fmt.Sprint(now + 60000)} {
		line := fmt.Sprintf(`{"event":"bounded_ready","cleanup_due_ms":"%s","keys_transmitter":false}`, due)
		if _, err := pdUnkeyReadiness([]byte(line), 15000); err == nil {
			t.Fatal("invalid deadline accepted", due)
		}
	}
	if _, err := pdUnkeyReadiness([]byte(valid), 0); err == nil {
		t.Fatal("bounded mode accepted as lifetime mode")
	}
	lifetime := []byte(`{"event":"ready","keys_transmitter":false}`)
	if _, err := pdUnkeyReadiness(lifetime, 0); err != nil {
		t.Fatal(err)
	}
	if _, err := pdUnkeyReadiness(lifetime, 15000); err == nil {
		t.Fatal("lifetime mode accepted as bounded mode")
	}
}

func boundedFixture(t *testing.T, delay time.Duration, prefix, suffix string) string {
	t.Helper()
	now, err := pdMonotonicMS()
	if err != nil {
		t.Fatal(err)
	}
	return fakePDProgram(t, "bounded", fmt.Sprintf("[ \"$#\" = 5 ] && [ \"$5\" = 15000 ] || exit 9\n%s\necho '{\"event\":\"bounded_ready\",\"cleanup_due_ms\":\"%d\",\"keys_transmitter\":false}'\n%s", prefix, now+delay.Milliseconds(), suffix))
}

func TestPDBoundedLaunchLifecycle(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("unprivileged launcher fixture required")
	}
	for _, action := range []string{"close", "cancel", "exit", "deadline"} {
		t.Run(action, func(t *testing.T) {
			api, trigger, err := os.Pipe()
			if err != nil {
				t.Fatal(err)
			}
			defer api.Close()
			defer trigger.Close()
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			suffix := "exec cat <&4 >/dev/null"
			if action == "exit" {
				suffix = "exec cat <&3 >/dev/null"
			}
			b, err := startPDBoundedUnkey(ctx, boundedFixture(t, 10*time.Second, "", suffix), api, 1, 2, 15000)
			if err != nil {
				t.Fatal(err)
			}
			defer b.close()
			if !b.observing() {
				t.Fatal("helper unavailable immediately")
			}
			due := b.guard.cleanupDue
			switch action {
			case "close":
				b.close()
			case "cancel":
				cancel()
			case "exit":
				trigger.Close()
			case "deadline":
				// Inject expiry into this private fixture; a production deadline is immutable.
				b.guard.cleanupDue = 1
				if b.observing() {
					t.Fatal("expired helper remained available")
				}
			}
			waitCtx, finish := context.WithTimeout(context.Background(), 2*time.Second)
			defer finish()
			if err := b.guard.wait(waitCtx); err != nil {
				t.Fatal("child not reaped", err)
			}
			if b.observing() {
				t.Fatal("ended helper remained available")
			}
			b.guard.cleanupDue = due
			if b.observing() {
				t.Fatal("retired observation revived")
			}
		})
	}
}

func TestPDBoundedStartupCancellation(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("unprivileged launcher fixture required")
	}
	api, trigger, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer api.Close()
	defer trigger.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	// This fixture blocks before readiness; exec ensures no grandchild owns stdout.
	start := time.Now()
	b, err := startPDBoundedUnkey(ctx, fakePDProgram(t, "pending", "exec cat <&3"), api, 1, 2, 15000)
	if err == nil || b != nil {
		t.Fatal("canceled startup succeeded")
	}
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatal("startup did not reach cancellation", err)
	}
	if time.Since(start) > time.Second {
		t.Fatal("startup cancellation did not interrupt readiness")
	}
	for _, duration := range []int{-1, 0, 15001} {
		if b, err := startPDBoundedUnkey(context.Background(), "/does-not-exist", api, 1, 2, duration); b != nil || err == nil {
			t.Fatal("invalid duration accepted")
		}
	}
}

func TestPDBoundedLateReadiness(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("unprivileged launcher fixture required")
	}
	api, err := os.Open(os.DevNull)
	if err != nil {
		t.Fatal(err)
	}
	defer api.Close()
	program := boundedFixture(t, -time.Second, "", "exec cat <&4 >/dev/null")
	if b, err := startPDBoundedUnkey(context.Background(), program, api, 1, 2, 15000); b != nil || err == nil {
		t.Fatal("expired readiness admitted")
	}
}
