//go:build linux

package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

func batchCaptureReply(id uint64, offset, count, state uint32) []byte {
	legacy := captureReply(id, offset, state)
	b := make([]byte, len(legacy)-aetherPDCaptureHeader+aetherPDCaptureBatchHeader)
	copy(b, legacy[:aetherPDCaptureHeader])
	copy(b, "PDIQ0005")
	copy(b[aetherPDCaptureBatchHeader:], legacy[aetherPDCaptureHeader:])
	index := offset / aetherPDCaptureSamples
	binary.BigEndian.PutUint64(b[48:], 1000000000+uint64(index)*85000000)
	binary.BigEndian.PutUint64(b[56:], 1085000000+uint64(index)*85000000)
	binary.BigEndian.PutUint32(b[68:], 7)
	binary.BigEndian.PutUint64(b[72:], 31)
	binary.BigEndian.PutUint32(b[80:], 1190)
	binary.BigEndian.PutUint32(b[84:], 3)
	binary.BigEndian.PutUint32(b[88:], 0x40000002)
	binary.BigEndian.PutUint32(b[92:], 7)
	binary.BigEndian.PutUint32(b[96:], index)
	binary.BigEndian.PutUint32(b[100:], count)
	return b
}

func TestPDCaptureBatchParserBounds(t *testing.T) {
	good := batchCaptureReply(9, 3*aetherPDCaptureSamples, 4, 2)
	for n := 0; n < len(good); n++ {
		if _, _, err := parseAetherPDCaptureBatch(good[:n], 9, 3*aetherPDCaptureSamples, 4); err == nil {
			t.Fatalf("accepted prefix %d", n)
		}
	}
	if _, _, err := parseAetherPDCapture(good, 9, 0); err == nil {
		t.Fatal("legacy reader accepted v5")
	}
	for _, args := range [][2]uint32{{0, 4}, {65536, 4}, {49153, 4}, {49152, 3}, {49152, 5}, {49152, 0}} {
		if _, _, err := parseAetherPDCaptureBatch(good, 9, args[0], args[1]); err == nil {
			t.Fatalf("accepted offset/count %v", args)
		}
	}
	for _, edit := range []struct {
		offset int
		value  uint32
	}{
		{0, 0}, {12, 10}, {16, 0}, {16, 6}, {20, 0}, {24, 1023}, {28, 1},
		{68, 8}, {80, 199}, {84, 5}, {88, 0}, {92, 0}, {96, 2}, {100, 3},
	} {
		bad := append([]byte(nil), good...)
		binary.BigEndian.PutUint32(bad[edit.offset:], edit.value)
		if _, _, err := parseAetherPDCaptureBatch(bad, 9, 49152, 4); err == nil {
			t.Fatalf("accepted changed field %d/%d", edit.offset, edit.value)
		}
	}
	for _, bits := range []uint32{0x7f800000, 0xff800000, 0x7fc00000} {
		bad := append([]byte(nil), good...)
		binary.LittleEndian.PutUint32(bad[len(bad)-4:], bits)
		if _, _, err := parseAetherPDCaptureBatch(bad, 9, 49152, 4); err == nil {
			t.Fatal("accepted nonfinite IQ")
		}
	}
	if _, _, err := parseAetherPDCaptureBatch(append(good, 0), 9, 49152, 4); err == nil {
		t.Fatal("accepted trailing byte")
	}
	for _, state := range []uint32{1, 2, 3, 4, 5} {
		_, got, err := parseAetherPDCaptureBatch(batchCaptureReply(9, 49152, 4, state), 9, 49152, 4)
		if err != nil || got != state {
			t.Fatal(state, got, err)
		}
	}
	for _, version := range []string{"PDIQ0001", "PDIQ0002", "PDIQ0003", "PDIQ0004"} {
		bad := append([]byte(nil), good...)
		copy(bad, version)
		if _, _, err := parseAetherPDCaptureBatch(bad, 9, 49152, 4); err == nil {
			t.Fatal("accepted", version)
		}
	}
}

func TestPDCaptureBatchAssembly(t *testing.T) {
	for count := uint32(1); count <= 4; count++ {
		t.Run(fmt.Sprint(count), func(t *testing.T) {
			calls := 0
			pending := make(map[uint32]bool)
			query := func(ctx context.Context, op byte, id uint64, offset uint32) ([]byte, uint64, error) {
				calls++
				if calls == 1 {
					if op != 'G' || id != 0 || offset != count {
						t.Fatal("wrong begin", op, id, offset)
					}
					id, offset = 9, 0
				} else if op != 'H' || id != 9 {
					t.Fatal("wrong read", op, id)
				}
				state := uint32(2)
				if offset%aetherPDCaptureSamples == 0 && !pending[offset] {
					pending[offset], state = true, 1
				}
				return batchCaptureReply(id, offset, count, state), id, nil
			}
			frames, err := readPDCaptureBatch(context.Background(), count, query)
			if err != nil || len(frames) != int(count) || calls != int(count)*17 {
				t.Fatal(len(frames), calls, err)
			}
			for index, frame := range frames {
				if len(frame.IQ) != 8*aetherPDCaptureSamples || frame.FirstReceiveNS != 1000000000+uint64(index)*85000000 {
					t.Fatal("incorrect frame", index, len(frame.IQ))
				}
				for i := 0; i < len(frame.IQ); i += 8 {
					if binary.LittleEndian.Uint32(frame.IQ[i:]) != 0x3f000000 || binary.LittleEndian.Uint32(frame.IQ[i+4:]) != 0xbe800000 {
						t.Fatal("sample changed")
					}
				}
			}
			if count > 1 {
				frames[0].IQ[0] = 1
				if frames[1].IQ[0] != 0 {
					t.Fatal("frames share writable samples")
				}
			}
		})
	}
}

func TestPDCaptureBatchRejectsMixedFrames(t *testing.T) {
	// Valid but different source fields must fail both within and between frames.
	for _, field := range []struct {
		offset int
		value  uint64
		wide   bool
	}{
		{28, 48000, false}, {32, 7160000, true}, {40, 8, true}, {64, 0x40000002, false},
		{68, 5, false}, {72, 32, true}, {80, 200, false}, {84, 4, false}, {88, 0x40000003, false}, {92, 8, false},
	} {
		for _, position := range []uint32{1024, 16384, 64512} {
			t.Run(fmt.Sprintf("field%d_at%d", field.offset, position), func(t *testing.T) {
				query := func(_ context.Context, op byte, id uint64, offset uint32) ([]byte, uint64, error) {
					if op == 'G' {
						id, offset = 9, 0
					}
					b := batchCaptureReply(id, offset, 4, 2)
					if offset == position || (position%aetherPDCaptureSamples == 0 && offset >= position) {
						if field.wide {
							binary.BigEndian.PutUint64(b[field.offset:], field.value)
						} else {
							binary.BigEndian.PutUint32(b[field.offset:], uint32(field.value))
						}
					}
					return b, id, nil
				}
				frames, err := readPDCaptureBatch(context.Background(), 4, query)
				if err == nil || frames != nil {
					t.Fatal("mixed or partial batch escaped", len(frames), err)
				}
			})
		}
	}
}

func TestPDCaptureBatchFailuresReturnNoFrames(t *testing.T) {
	for _, failure := range []string{"query", "pending", "invalid", "expired", "unavailable", "clock_reverse", "gap", "timestamp_change", "unkeyed", "no_tones", "no_binding", "wrong_id"} {
		t.Run(failure, func(t *testing.T) {
			query := func(_ context.Context, op byte, id uint64, offset uint32) ([]byte, uint64, error) {
				if op == 'G' {
					id, offset = 9, 0
				}
				b := batchCaptureReply(id, offset, 4, 2)
				if offset == 32768+1024 && (failure == "pending" || failure == "timestamp_change") {
					if failure == "pending" {
						b = batchCaptureReply(id, offset, 4, 1)
					} else {
						binary.BigEndian.PutUint64(b[56:], 1255000001)
					}
				}
				if offset == 32768 {
					switch failure {
					case "query":
						return nil, 0, errors.New("injected read failure")
					case "invalid":
						b = batchCaptureReply(id, offset, 4, 3)
					case "expired":
						b = batchCaptureReply(id, offset, 4, 4)
					case "unavailable":
						b = batchCaptureReply(id, offset, 4, 5)
					case "clock_reverse":
						binary.BigEndian.PutUint64(b[48:], 1169999999)
					case "gap":
						binary.BigEndian.PutUint64(b[48:], 1270000001)
						binary.BigEndian.PutUint64(b[56:], 1355000001)
					case "unkeyed":
						binary.BigEndian.PutUint32(b[68:], 0)
					case "no_tones":
						binary.BigEndian.PutUint32(b[68:], 3)
					case "no_binding":
						clear(b[84:96])
					case "wrong_id":
						binary.BigEndian.PutUint64(b[8:], 10)
					}
				}
				return b, id, nil
			}
			frames, err := readPDCaptureBatch(context.Background(), 4, query)
			if err == nil || frames != nil {
				t.Fatal("failed or partial batch escaped", len(frames), err)
			}
		})
	}
}

func TestPDCaptureBatchCancellation(t *testing.T) {
	for _, when := range []string{"before_begin", "pending", "after_frames", "last_chunk"} {
		t.Run(when, func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			calls := 0
			if when == "before_begin" {
				cancel()
			}
			query := func(_ context.Context, op byte, id uint64, offset uint32) ([]byte, uint64, error) {
				calls++
				if op == 'G' {
					id, offset = 9, 0
				}
				state := uint32(2)
				if when == "pending" {
					state = 1
					cancel()
				}
				if (when == "after_frames" && offset == 32768) || (when == "last_chunk" && offset == 64512) {
					cancel()
				}
				return batchCaptureReply(id, offset, 4, state), id, nil
			}
			frames, err := readPDCaptureBatch(ctx, 4, query)
			if !errors.Is(err, context.Canceled) || frames != nil || (when == "before_begin" && calls != 0) {
				t.Fatal(calls, len(frames), err)
			}
		})
	}
	for _, count := range []uint32{0, 5, ^uint32(0)} {
		frames, err := readPDCaptureBatch(context.Background(), count, func(context.Context, byte, uint64, uint32) ([]byte, uint64, error) {
			t.Fatal("invalid count reached transport")
			return nil, 0, nil
		})
		if err == nil || frames != nil {
			t.Fatal("invalid count accepted", count)
		}
	}
}

func TestPDCaptureBatchNativeQueryBounds(t *testing.T) {
	p, _ := privateStatusFixture(t)
	for _, request := range []struct {
		op    byte
		id    uint64
		value uint32
	}{{'G', 0, 0}, {'G', 0, 5}, {'H', 0, 0}, {'H', 9, 1}, {'H', 9, 65536}, {'H', 9, ^uint32(0)}} {
		data, id, err := p.readNativeQuery(context.Background(), request.op, request.id, request.value)
		if err == nil || data != nil || id != 0 || p.statusNonce != 0 {
			t.Fatal("invalid request reached native transport", request, err)
		}
	}
	close(p.done)
	if frames, err := p.readNativeCaptureBatch(context.Background(), 4); err == nil || frames != nil || p.statusNonce != 0 {
		t.Fatal("stopped owner reached native transport", err)
	}
}

// Opt-in integration with our native collector and real G/H socket codec.
// Build capture_batch_fixture.c + pd_capture.c + pd_status.c and set the path.
func TestPDCaptureBatchNativeFixture(t *testing.T) {
	path := os.Getenv("AETHER_PD_CAPTURE_BATCH_FIXTURE")
	if path == "" {
		t.Skip("explicit native C batch fixture not provided")
	}
	for count := uint32(1); count <= 4; count++ {
		t.Run(fmt.Sprint(count), func(t *testing.T) {
			p, peer := privateStatusFixture(t)
			file, err := peer.File()
			if err != nil {
				t.Fatal(err)
			}
			defer file.Close()
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			cmd := exec.CommandContext(ctx, path)
			cmd.ExtraFiles = []*os.File{file}
			if err := cmd.Start(); err != nil {
				t.Fatal(err)
			}
			peer.Close()
			frames, readErr := p.readNativeCaptureBatch(ctx, count)
			if readErr != nil {
				cancel()
			}
			waitErr := cmd.Wait()
			if readErr != nil || waitErr != nil || len(frames) != int(count) {
				t.Fatal(len(frames), readErr, waitErr)
			}
			for index, frame := range frames {
				if len(frame.IQ) != 8*aetherPDCaptureSamples || frame.Rate != 48000 || frame.CenterHz != 7150000 ||
					frame.Generation != 7 || frame.Flags != 3 || frame.Binding == nil || frame.Binding.Channel != 3 ||
					frame.Measurement == nil || frame.Measurement.SourceID != "000000000000001f" || !frame.Measurement.Tones ||
					frame.ToneConfig == nil || frame.ToneConfig.SpacingHz != 1190 {
					t.Fatal("native metadata changed", index)
				}
				for i := 0; i < aetherPDCaptureSamples; i++ {
					sample := uint32(index*aetherPDCaptureSamples + i)
					if binary.LittleEndian.Uint32(frame.IQ[8*i:]) != 0x3e000000+sample ||
						binary.LittleEndian.Uint32(frame.IQ[8*i+4:]) != 0xbe000000+sample {
						t.Fatalf("native packet/frame boundary lost sample %d", sample)
					}
				}
			}
		})
	}
}

func TestPDCaptureBatchNativePublicFixture(t *testing.T) {
	path := os.Getenv("AETHER_PD_CAPTURE_BATCH_FIXTURE")
	if path == "" {
		t.Skip("explicit native C batch fixture not provided")
	}
	p, peer := privateStatusFixture(t)
	file, err := peer.File()
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, path)
	cmd.ExtraFiles = []*os.File{file}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	peer.Close()
	n := newTestNode(t, "owner")
	n.pdSession = batchSessionPair(t, p.captureBatchOperation)
	h := NewAPI(n)
	response := call(t, h, "POST", "/v4/aetherpd/capture/4", "192.168.50.20:4000", "owner", "")
	if response.Code != 200 {
		cancel()
	}
	waitErr := cmd.Wait()
	var manifest pdCaptureBatchManifest
	if waitErr != nil || response.Code != 200 || json.Unmarshal(response.Body.Bytes(), &manifest) != nil || !manifest.valid(4) {
		t.Fatal(waitErr, response.Code, response.Body.String())
	}
	directory := os.Getenv("AETHER_PD_CAPTURE_BATCH_FIXTURE_DIR")
	if directory != "" {
		if err := os.MkdirAll(directory, 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(directory, "manifest.json"), response.Body.Bytes(), 0600); err != nil {
			t.Fatal(err)
		}
	}
	for frame := 0; frame < 4; frame++ {
		if manifest.Frames[frame].SampleRate != 48000 || manifest.Frames[frame].Source.Generation != "7" ||
			manifest.Frames[frame].ToneConfig.SpacingHz != 1190 || manifest.Frames[frame].Binding.Channel != 3 {
			t.Fatal("native identity lost")
		}
		for chunk := 0; chunk < 32; chunk++ {
			path := fmt.Sprintf("/v4/aetherpd/capture/%s/%d/%d", manifest.CaptureID, frame, chunk)
			r := call(t, h, "GET", path, "192.168.50.20:4000", "owner", "")
			if directory != "" {
				if err := os.WriteFile(filepath.Join(directory, fmt.Sprintf("chunk-%d-%d.json", frame, chunk)), r.Body.Bytes(), 0600); err != nil {
					t.Fatal(err)
				}
			}
			var payload pdCaptureBatchChunk
			if r.Code != 200 || json.Unmarshal(r.Body.Bytes(), &payload) != nil || payload.Frame != frame || payload.Offset != chunk*512 || len(payload.IQ) != 4096 {
				t.Fatal(frame, chunk, r.Code)
			}
			for i := 0; i < 512; i++ {
				index := uint32(frame*16384 + chunk*512 + i)
				if binary.LittleEndian.Uint32(payload.IQ[8*i:]) != 0x3e000000+index || binary.LittleEndian.Uint32(payload.IQ[8*i+4:]) != 0xbe000000+index {
					t.Fatal("public IQ changed", index)
				}
			}
		}
	}
}
