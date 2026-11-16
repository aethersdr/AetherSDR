//go:build linux

package main

import (
	"context"
	"errors"
	"io"
	"os"
	"strings"
	"testing"
	"time"
)

var testUnkeyIdentity = pdStartIdentity{GUIClientID: "11111111-2222-4333-8444-555555555555", GUIHandle: 0x11223344}

func TestPDTestUnkeyBind(t *testing.T) {
	const tail = "S01020304|interlock state=READY\nR910000|0|0x11223344\n"
	for _, first := range []string{"V1.4.0.0\r\nH01020304\nR970101|0|0x11223344\n", "R970101|0000|0x11223344\nH01020304\n"} {
		wire := &pdObservationWire{Reader: strings.NewReader(first + tail)}
		client, err := bindPDTestUnkeyAPI(context.Background(), wire, testUnkeyIdentity)
		if err != nil || client != 0x01020304 {
			t.Fatal(client, err)
		}
		remaining, _ := io.ReadAll(wire)
		if string(remaining) != tail {
			t.Fatal("handoff consumed guardian input", string(remaining))
		}
		want := "C970101|client bind client_id=" + testUnkeyIdentity.GUIClientID + "\nC910000|client bind client_id=" + testUnkeyIdentity.GUIClientID + "\n"
		if wire.writes.String() != want {
			t.Fatal("unexpected commands", wire.writes.String())
		}
	}
}

func TestPDTestUnkeyBindRefusals(t *testing.T) {
	for _, frames := range []string{
		"H01020304\nR970101|0|0x55667788\n", "H01020304\nR970101|50000001|0x11223344\n",
		"H01020304\nR970101|0|\n", "H01020304\nR910000|0|0x11223344\n",
		"H01020304\nR970101|0|0x11223344|extra\n", "H01020304\nR970101|+0|0x11223344\n",
		"H01020304\nR970101|0|0x+11223344\n", "H00000000\n", "H11223344\n", "H+1\n", "H100000000\n",
		"H01020304\nH01020304\n", "R970101|0|0x11223344\nR970101|0|0x11223344\n",
		"H01020304\n", "H0102\x00304\n", "H0102\r0304\n", "\n", "Xunexpected\n",
		"V" + strings.Repeat("x", 4096) + "\n", strings.Repeat("Vx\n", 22000),
	} {
		wire := &pdObservationWire{Reader: strings.NewReader(frames)}
		if _, err := bindPDTestUnkeyAPI(context.Background(), wire, testUnkeyIdentity); err == nil {
			t.Fatal("bad bind accepted", frames[:min(len(frames), 100)])
		}
		if strings.Contains(wire.writes.String(), "C910000") {
			t.Fatal("guardian binding issued after refusal")
		}
	}
	wire := &pdObservationWire{Reader: strings.NewReader("")}
	if _, err := bindPDTestUnkeyAPI(context.Background(), wire, pdStartIdentity{GUIClientID: "bad\ncommand", GUIHandle: 1}); err == nil || wire.writes.Len() != 0 {
		t.Fatal("invalid identity reached transport")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := bindPDTestUnkeyAPI(ctx, wire, testUnkeyIdentity); err == nil || wire.writes.Len() != 0 {
		t.Fatal("canceled bind reached transport")
	}
}

func testUnkeyHelper(t *testing.T, ctx context.Context) *pdBoundedUnkey {
	t.Helper()
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { r.Close(); w.Close() })
	now, err := pdMonotonicMS()
	if err != nil {
		t.Fatal(err)
	}
	b := &pdBoundedUnkey{life: w, guard: &aetherPDUnkey{done: make(chan struct{}), cleanupDue: now + 15000}}
	stop := context.AfterFunc(ctx, b.close)
	t.Cleanup(func() { stop(); b.close() })
	return b
}

func awaitTestUnkey(t *testing.T, r *pdTestUnkeyRun) {
	t.Helper()
	select {
	case <-r.done:
	case <-time.After(time.Second):
		t.Fatal("run not retired")
	}
}

func TestPDTestUnkeySerialization(t *testing.T) {
	var s pdTestUnkeys
	var oldHelper *pdBoundedUnkey
	r, err := s.start(context.Background(), func(ctx context.Context) (*pdBoundedUnkey, error) {
		oldHelper = testUnkeyHelper(t, ctx)
		return oldHelper, nil
	})
	if err != nil || !r.observing() {
		t.Fatal("run not available", err)
	}
	never := func(context.Context) (*pdBoundedUnkey, error) {
		t.Fatal("overlapping helper launched")
		return nil, nil
	}
	if next, err := s.start(context.Background(), never); next != nil || err == nil {
		t.Fatal("active overlap accepted")
	}
	r.stop()
	if r.observing() || !oldHelper.retired.Load() {
		t.Fatal("stop did not retire helper")
	}
	if next, err := s.start(context.Background(), never); next != nil || err == nil {
		t.Fatal("unreaped overlap accepted")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if s.wait(ctx) == nil {
		t.Fatal("wait claimed unobserved termination")
	}
	close(oldHelper.guard.done)
	awaitTestUnkey(t, r)
	var current *pdBoundedUnkey
	next, err := s.start(context.Background(), func(ctx context.Context) (*pdBoundedUnkey, error) {
		current = testUnkeyHelper(t, ctx)
		return current, nil
	})
	if err != nil || !next.observing() {
		t.Fatal("fresh explicit start refused", err)
	}
	r.stop()
	if !next.observing() {
		t.Fatal("stale run stopped replacement")
	}
	s.close()
	if next.observing() || !current.retired.Load() {
		t.Fatal("session closure retained run")
	}
	close(current.guard.done)
	awaitTestUnkey(t, next)
	if _, err := s.start(context.Background(), never); err == nil {
		t.Fatal("closed session revived")
	}
}

func TestPDTestUnkeyPendingSessionStop(t *testing.T) {
	var s pdTestUnkeys
	entered := make(chan context.Context, 1)
	release := make(chan struct{})
	type result struct {
		run *pdTestUnkeyRun
		err error
	}
	finished := make(chan result, 1)
	// Allocate fixture on the test goroutine; the pending launch publishes it late.
	helper := testUnkeyHelper(t, context.Background())
	go func() {
		r, err := s.start(context.Background(), func(ctx context.Context) (*pdBoundedUnkey, error) { entered <- ctx; <-release; return helper, nil })
		finished <- result{r, err}
	}()
	ctx := <-entered
	s.close()
	if ctx.Err() == nil {
		t.Fatal("session stop did not cancel pending bind")
	}
	close(release)
	got := <-finished
	if got.err == nil || got.run == nil || got.run.observing() || !helper.retired.Load() {
		t.Fatal("late readiness escaped stop", got.err)
	}
	close(helper.guard.done)
	awaitTestUnkey(t, got.run)
}

func TestPDTestUnkeyFailedChildRetained(t *testing.T) {
	var s pdTestUnkeys
	helper := testUnkeyHelper(t, context.Background())
	r, err := s.start(context.Background(), func(context.Context) (*pdBoundedUnkey, error) { return helper, errors.New("termination pending") })
	if err == nil || r == nil || r.observing() || !helper.retired.Load() {
		t.Fatal("failed child not retired and retained")
	}
	if _, err := s.start(context.Background(), func(context.Context) (*pdBoundedUnkey, error) {
		t.Fatal("replacement while old child lives")
		return nil, nil
	}); err == nil {
		t.Fatal("failed child cleared too soon")
	}
	close(helper.guard.done)
	awaitTestUnkey(t, r)
	r, err = s.start(context.Background(), func(context.Context) (*pdBoundedUnkey, error) { return nil, errors.New("bind refused") })
	if err == nil || r == nil {
		t.Fatal("missing failed attempt handle")
	}
	awaitTestUnkey(t, r)
	if s.wait(context.Background()) != nil {
		t.Fatal("no-child failure remained busy")
	}
}

func TestPDTestUnkeyOwnedProcessStop(t *testing.T) {
	read, write, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer read.Close()
	defer write.Close()
	p := &aetherPDProcess{owner: write, done: make(chan struct{})}
	helper := testUnkeyHelper(t, context.Background())
	r, err := p.testUnkeys.start(context.Background(), func(context.Context) (*pdBoundedUnkey, error) { return helper, nil })
	if err != nil {
		t.Fatal(err)
	}
	p.Stop()
	if r.observing() || !helper.retired.Load() {
		t.Fatal("DSP owner stop did not retire test helper")
	}
	close(helper.guard.done)
	awaitTestUnkey(t, r)
	if _, err := p.startTestUnkey(context.Background(), 15000); err == nil {
		t.Fatal("incomplete session admitted")
	}
}

func TestPDTestUnkeyRealLauncherSupervision(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("unprivileged launcher fixture required")
	}
	var s pdTestUnkeys
	api, err := os.Open(os.DevNull)
	if err != nil {
		t.Fatal(err)
	}
	defer api.Close()
	program := boundedFixture(t, 10*time.Second, "", "exec cat <&4 >/dev/null")
	r, err := s.start(context.Background(), func(ctx context.Context) (*pdBoundedUnkey, error) {
		return startPDBoundedUnkey(ctx, program, api, 1, 2, 15000)
	})
	if err != nil || !r.observing() {
		t.Fatal("actual launcher was not supervised", err)
	}
	s.close()
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if err := s.wait(ctx); err != nil {
		t.Fatal("owned child not reaped", err)
	}
	awaitTestUnkey(t, r)
	if r.observing() {
		t.Fatal("supervised child available after stop")
	}
}

type cancelBindWire struct {
	*pdObservationWire
	cancel context.CancelFunc
	left   int
}

func (w *cancelBindWire) Read(b []byte) (int, error) {
	n, err := w.pdObservationWire.Read(b)
	w.left -= n
	if w.left == 0 {
		w.cancel()
	}
	return n, err
}

func TestPDTestUnkeyCancelAtBindBarrier(t *testing.T) {
	frames := "H01020304\nR970101|0|0x11223344\n"
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	wire := &cancelBindWire{pdObservationWire: &pdObservationWire{Reader: strings.NewReader(frames)}, cancel: cancel, left: len(frames)}
	if _, err := bindPDTestUnkeyAPI(ctx, wire, testUnkeyIdentity); !errors.Is(err, context.Canceled) {
		t.Fatal("canceled bind completed", err)
	}
	if strings.Contains(wire.writes.String(), "C910000") {
		t.Fatal("handoff binding escaped cancellation")
	}
}
