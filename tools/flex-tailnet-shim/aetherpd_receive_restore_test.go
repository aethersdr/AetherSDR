package main

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"net"
	"strings"
	"testing"
	"time"
)

func receiveRestoreFixture() (pdReceiveOriginal, pdReceiveView) {
	r, v, _ := receiveRecoveryFixture()
	v.observedAt = time.Now()
	v.complete = false // Existing receivers require no allocation-capacity claim.
	v.slices[0] = pdReceiveSlice{present: true, owner: r.original.guiHandle, panID: r.original.panID, frequencyHz: r.original.settings.frequencyHz, mode: r.original.settings.mode, daxChannel: 1}
	r.original.settings.agcThreshold = 65
	r.original.settings.agcOffLevel = 10
	r.original.settings.nbLevel = 50
	return r.original, v
}

func TestPDReceiveRestoreUsesOnlyCapturedReceiveSettings(t *testing.T) {
	original, view := receiveRestoreFixture()
	writes := []string{}
	reads := 0
	adapter := pdReceiveRestoreIO{
		observe: func(context.Context) (pdReceiveView, error) { reads++; return view, nil },
		write:   func(_ context.Context, text string) error { writes = append(writes, text); return nil },
		readback: func(context.Context) (pdReceiveObservation, error) {
			return pdReceiveObservation{original: original, view: view}, nil
		},
	}
	if err := restoreExistingPDReceive(context.Background(), original, adapter, func() bool { return true }); err != nil {
		t.Fatal(err)
	}
	if reads != 2 || len(writes) != 2 {
		t.Fatalf("unexpected operation count: reads%d writes%v", reads, writes)
	}
	want := "slice set 0 agc_mode=med agc_threshold=65 agc_off_level=10 audio_level=50 audio_pan=50 audio_mute=0 dax=1 rit_on=0 rit_freq=0 squelch=1 squelch_level=20 nr=0 nr_level=50 nb=0 nb_level=50 anf=0 apf=0 apf_level=30"
	if writes[0] != "filt 0 100 2800" || writes[1] != want {
		t.Fatalf("wrong receive commands: %v", writes)
	}
}

func TestPDReceiveRestoreRefusesChangedReceiverBeforeEachWrite(t *testing.T) {
	for name, alter := range map[string]func(*pdReceiveView){
		"absent":          func(v *pdReceiveView) { v.slices[0] = pdReceiveSlice{} },
		"foreign":         func(v *pdReceiveView) { v.slices[0].owner++ },
		"retuned":         func(v *pdReceiveView) { v.slices[0].frequencyHz++ },
		"mode changed":    func(v *pdReceiveView) { v.slices[0].mode = "LSB" },
		"different pan":   func(v *pdReceiveView) { v.slices[0].panID++ },
		"PTT":             func(v *pdReceiveView) { v.ready = false; v.source = "SW" },
		"stale":           func(v *pdReceiveView) { v.observedAt = time.Now().Add(-2 * time.Second) },
		"GUI reused":      func(v *pdReceiveView) { v.clientID = "99999999-2222-4333-8444-555555555555" },
		"DAX reassigned":  func(v *pdReceiveView) { v.slices[1].daxChannel = 1 },
		"own DAX changed": func(v *pdReceiveView) { v.slices[0].daxChannel = 2 },
	} {
		for _, beforeWrite := range []int{0, 1} {
			t.Run(name+string(rune('0'+beforeWrite)), func(t *testing.T) {
				original, view := receiveRestoreFixture()
				writes := 0
				reads := 0
				adapter := pdReceiveRestoreIO{
					observe: func(context.Context) (pdReceiveView, error) {
						next := view
						if reads == beforeWrite {
							alter(&next)
						}
						reads++
						return next, nil
					},
					write: func(context.Context, string) error { writes++; return nil },
					readback: func(context.Context) (pdReceiveObservation, error) {
						t.Fatal("readback after rejected observation")
						return pdReceiveObservation{}, nil
					},
				}
				if err := restoreExistingPDReceive(context.Background(), original, adapter, func() bool { return true }); err == nil {
					t.Fatal("changed receiver accepted")
				}
				if writes != beforeWrite {
					t.Fatalf("wrote after identity changed: %d", writes)
				}
			})
		}
	}
}

func TestPDReceiveRestoreCancellationRevocationAndReadback(t *testing.T) {
	for _, failure := range []string{"cancel", "revoke", "write rejected", "wrong readback"} {
		t.Run(failure, func(t *testing.T) {
			original, view := receiveRestoreFixture()
			writes := 0
			authorized := true
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			adapter := pdReceiveRestoreIO{
				observe: func(context.Context) (pdReceiveView, error) { return view, nil },
				write: func(context.Context, string) error {
					writes++
					switch failure {
					case "cancel":
						cancel()
					case "revoke":
						authorized = false
					case "write rejected":
						return errors.New("rejected")
					}
					return nil
				},
				readback: func(context.Context) (pdReceiveObservation, error) {
					o := original
					o.settings.nbLevel = 0
					return pdReceiveObservation{original: o, view: view}, nil
				},
			}
			if err := restoreExistingPDReceive(ctx, original, adapter, func() bool { return authorized }); err == nil {
				t.Fatal("unconfirmed restore reported success")
			}
			want := 1
			if failure == "wrong readback" {
				want = 2
			}
			if writes != want {
				t.Fatalf("unexpected writes after failure: %d", writes)
			}
		})
	}
}

type pdReceiveReplyConn struct {
	*strings.Reader
	writes bytes.Buffer
	short  bool
}

func (c *pdReceiveReplyConn) Write(p []byte) (int, error) {
	if c.short {
		return len(p) - 1, nil
	}
	return c.writes.Write(p)
}
func (*pdReceiveReplyConn) Close() error                     { return nil }
func (*pdReceiveReplyConn) LocalAddr() net.Addr              { return nil }
func (*pdReceiveReplyConn) RemoteAddr() net.Addr             { return nil }
func (*pdReceiveReplyConn) SetDeadline(time.Time) error      { return nil }
func (*pdReceiveReplyConn) SetReadDeadline(time.Time) error  { return nil }
func (*pdReceiveReplyConn) SetWriteDeadline(time.Time) error { return nil }

func TestPDReceiveWriterRequiresExactSuccessfulAcknowledgement(t *testing.T) {
	for _, reply := range []string{"R41|0|\n", "R41|50000001|\n", "R42|0|\n", "Rbad|0|\n", "R41|0\n", strings.Repeat("x", 4098) + "\n"} {
		conn := &pdReceiveReplyConn{Reader: strings.NewReader(reply)}
		writer := pdReceiveWriter{conn: conn, reader: bufio.NewReaderSize(conn, 4098), sequence: 40}
		err := writer.command(context.Background(), "filt 0 100 2800")
		if (err == nil) != (reply == "R41|0|\n") {
			t.Fatalf("incorrect acknowledgement result: %q %v", reply, err)
		}
		if conn.writes.String() != "C41|filt 0 100 2800\n" {
			t.Fatal("wrong wire command")
		}
	}
	conn := &pdReceiveReplyConn{Reader: strings.NewReader("R41|0|\n"), short: true}
	writer := pdReceiveWriter{conn: conn, reader: bufio.NewReader(conn), sequence: 40}
	if err := writer.command(context.Background(), "filt 0 100 2800"); err == nil {
		t.Fatal("short write accepted")
	}
}

func TestPDReceiveWriterRoutesOnlyMatchingReply(t *testing.T) {
	for _, test := range []struct {
		name, replies string
		success       bool
	}{
		{"older_success", "R40|0|old\nR41|0|current\n", true},
		{"unrelated_failure", "R5|50000001|unrelated\nR41|0|current\n", true},
		{"future_success", "R42|0|other\nR41|0|current\n", true},
		{"zero_sequence", "R0|0|other\nR41|0|current\n", true},
		{"matching_failure", "R40|0|old\nR41|50000001|refused\nR42|0|other\n", false},
		{"matching_absent", "R40|0|old\nR42|0|other\n", false},
		{"malformed_sequence", "Rbad|0|other\nR41|0|current\n", false},
		{"malformed_code", "R40|invalid|other\nR41|0|current\n", false},
		{"bounded_unrelated_flood", strings.Repeat("R40|0|old\n", 8192) + "R41|0|current\n", false},
	} {
		t.Run(test.name, func(t *testing.T) {
			conn := &pdReceiveReplyConn{Reader: strings.NewReader(test.replies)}
			writer := pdReceiveWriter{conn: conn, reader: bufio.NewReaderSize(conn, 4098), sequence: 40}
			body, err := writer.reply(context.Background(), "slice list")
			if (err == nil) != test.success || test.success && body != "current" {
				t.Fatalf("matching reply: body=%q error=%v", body, err)
			}
			if conn.writes.String() != "C41|slice list\n" {
				t.Fatal("reply routing issued extra commands", conn.writes.String())
			}
		})
	}
}
