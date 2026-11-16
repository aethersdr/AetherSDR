//go:build linux

package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"testing"
	"time"
)

func TestPrivateAetherPDCaptureNativePublicFixture(t *testing.T) {
	testPrivateAetherPDCapturePublic(t, pdCaptureBegin, 0)
}

func TestPrivateAetherPDCaptureNativePublicSourceFixture(t *testing.T) {
	testPrivateAetherPDCapturePublic(t, pdCaptureBeginWithSource, 0)
}

func TestPrivateAetherPDCaptureNativePublicToneFixture(t *testing.T) {
	for _, spacing := range []uint32{200, 1001, 1190, 2610, 10000} {
		t.Run(strconv.Itoa(int(spacing)), func(t *testing.T) { testPrivateAetherPDCapturePublic(t, pdCaptureBeginWithTones, spacing) })
	}
}

func testPrivateAetherPDCapturePublic(t *testing.T, mode byte, spacing uint32) {
	withSource := mode != pdCaptureBegin
	t.Helper()
	path := os.Getenv("AETHER_PD_CAPTURE_FIXTURE")
	if path == "" {
		t.Skip("explicit native C fixture not provided")
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
	if spacing != 0 {
		cmd.Args = append(cmd.Args, strconv.Itoa(int(spacing)))
	}
	cmd.ExtraFiles = []*os.File{file}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	peer.Close()
	n := newTestNode(t, "owner")
	if mode == pdCaptureBeginWithTones {
		node := &Node{pdProcess: p}
		n.pdSession = toneSessionPair(t, pdSessionOperations(node), pdSessionToneOperations(node))
	} else {
		n.pdSession, _ = pdSessionTestPair(t, pdSessionOperations(&Node{pdProcess: p}))
	}
	n.pdSession.captureSourceSupported = withSource
	h := NewAPI(n)
	endpoint := "/v1/aetherpd/capture"
	if withSource {
		endpoint = "/v2/aetherpd/capture"
	}
	if mode == pdCaptureBeginWithTones {
		endpoint = "/v3/aetherpd/capture"
	}
	response := call(t, h, "POST", endpoint, "192.168.50.20:4000", "owner", "")
	waitErr := cmd.Wait()
	var manifest pdCaptureManifest
	if waitErr != nil || response.Code != 200 || json.Unmarshal(response.Body.Bytes(), &manifest) != nil {
		t.Fatal(waitErr, response.Code, response.Body.String())
	}
	if manifest.SampleRate != 192000 || manifest.CenterHz != 7150000 || !manifest.Keyed || !manifest.Correcting ||
		manifest.Measurement == nil || manifest.Measurement.SourceID != "000000000000001f" || !manifest.Measurement.Tones {
		t.Fatal("native metadata changed")
	}
	if mode == pdCaptureBeginWithTones {
		if !manifest.ToneConfig.valid() || manifest.ToneConfig.SpacingHz != spacing || manifest.ToneConfig.SourceID != manifest.Measurement.SourceID {
			t.Fatal("native emitted profile lost at public boundary", response.Body.String())
		}
	} else if manifest.ToneConfig != nil {
		t.Fatal("legacy manifest changed")
	}
	directory := os.Getenv("AETHER_PD_CAPTURE_FIXTURE_DIR")
	if mode == pdCaptureBeginWithTones && directory != "" {
		directory = filepath.Join(directory, "tones-"+strconv.Itoa(int(spacing)))
	}
	if withSource {
		if !manifest.Source.valid() {
			t.Fatal("sample source identity missing")
		}
		if directory != "" {
			directory = filepath.Join(directory, "source")
			if err := os.MkdirAll(directory, 0700); err != nil {
				t.Fatal(err)
			}
		}
	}
	if directory != "" {
		if err := os.WriteFile(filepath.Join(directory, "manifest.json"), response.Body.Bytes(), 0600); err != nil {
			t.Fatal(err)
		}
	}
	var iq []byte
	for index := 0; index < 32; index++ {
		response = call(t, h, "GET", fmt.Sprintf("/v1/aetherpd/capture/%s/%d", manifest.CaptureID, index), "192.168.50.20:4000", "owner", "")
		var chunk pdCaptureChunk
		if response.Code != 200 || json.Unmarshal(response.Body.Bytes(), &chunk) != nil || chunk.Offset != 512*index {
			t.Fatal(response.Code, response.Body.String())
		}
		if directory != "" {
			if err := os.WriteFile(filepath.Join(directory, fmt.Sprintf("chunk-%d.json", index)), response.Body.Bytes(), 0600); err != nil {
				t.Fatal(err)
			}
		}
		iq = append(iq, chunk.IQ...)
	}
	if len(iq) != 8*aetherPDCaptureSamples {
		t.Fatal("incomplete native public capture")
	}
	for i := 0; i < len(iq); i += 8 {
		if binary.LittleEndian.Uint32(iq[i:]) != 0x3f000000 || binary.LittleEndian.Uint32(iq[i+4:]) != 0xbe800000 {
			t.Fatal("native public sample changed")
		}
	}
}

// Opt-in own-protocol integration: build capture_fixture.c with pd_capture.c
// and pd_status.c, then set AETHER_PD_CAPTURE_FIXTURE to the resulting binary.
func TestPrivateAetherPDCaptureNativeFixture(t *testing.T) {
	path := os.Getenv("AETHER_PD_CAPTURE_FIXTURE")
	if path == "" {
		t.Skip("explicit native C fixture not provided")
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
	result, readErr := p.readNativeCapture(ctx)
	waitErr := cmd.Wait()
	if readErr != nil || waitErr != nil {
		t.Fatal(readErr, waitErr)
	}
	if len(result.IQ) != 8*aetherPDCaptureSamples || result.Rate != 192000 || result.CenterHz != 7150000 || result.Generation != 7 || result.Flags != 3 {
		t.Fatal("incorrect native frame metadata")
	}
	for i := 0; i < len(result.IQ); i += 8 {
		if binary.LittleEndian.Uint32(result.IQ[i:]) != 0x3f000000 || binary.LittleEndian.Uint32(result.IQ[i+4:]) != 0xbe800000 {
			t.Fatal("native raw sample changed")
		}
	}
}

func captureReply(id uint64, offset, state uint32) []byte {
	size := aetherPDCaptureHeader
	if state == 2 {
		size += 8 * aetherPDCaptureChunk
	}
	b := make([]byte, size)
	copy(b, "PDIQ0001")
	binary.BigEndian.PutUint64(b[8:], id)
	binary.BigEndian.PutUint32(b[16:], state)
	binary.BigEndian.PutUint32(b[20:], offset)
	if state == 2 {
		binary.BigEndian.PutUint32(b[24:], aetherPDCaptureChunk)
	}
	binary.BigEndian.PutUint32(b[28:], 192000)
	binary.BigEndian.PutUint64(b[32:], 7150000)
	binary.BigEndian.PutUint64(b[40:], 7)
	binary.BigEndian.PutUint64(b[48:], 1000000000)
	binary.BigEndian.PutUint64(b[56:], 1085000000)
	binary.BigEndian.PutUint32(b[64:], 0x40000001)
	binary.BigEndian.PutUint32(b[68:], 3)
	for i := aetherPDCaptureHeader; i < len(b); i += 8 {
		binary.LittleEndian.PutUint32(b[i:], 0x3f000000)
		binary.LittleEndian.PutUint32(b[i+4:], 0xbe800000)
	}
	return b
}

func TestAetherPDCaptureParserBoundaries(t *testing.T) {
	good := captureReply(9, 0, 2)
	for n := 0; n < len(good); n++ {
		if _, _, err := parseAetherPDCapture(good[:n], 9, 0); err == nil {
			t.Fatalf("accepted prefix %d", n)
		}
	}
	c, state, err := parseAetherPDCapture(good, 9, 0)
	if err != nil || state != 2 || c.CenterHz != 7150000 || len(c.IQ) != 8192 {
		t.Fatal(c, state, err)
	}
	for _, edit := range []struct {
		offset int
		value  uint64
		wide   bool
	}{
		{8, 10, true}, {16, 6, false}, {20, 1, false}, {24, 1023, false}, {28, 1, false},
		{32, 0, true}, {40, 0, true}, {48, 1085000001, true}, {56, 3000000000, true},
		{64, 0, false}, {68, 2, false}, {68, 4, false},
	} {
		b := append([]byte(nil), good...)
		if edit.wide {
			binary.BigEndian.PutUint64(b[edit.offset:], edit.value)
		} else {
			binary.BigEndian.PutUint32(b[edit.offset:], uint32(edit.value))
		}
		if _, _, err := parseAetherPDCapture(b, 9, 0); err == nil {
			t.Fatalf("accepted changed field %d/%d", edit.offset, edit.value)
		}
	}
	bad := append([]byte(nil), good...)
	binary.LittleEndian.PutUint32(bad[len(bad)-4:], 0x7fc00000)
	if _, _, err := parseAetherPDCapture(bad, 9, 0); err == nil {
		t.Fatal("accepted NaN")
	}
	for _, state := range []uint32{1, 3, 4, 5} {
		if _, got, err := parseAetherPDCapture(captureReply(9, 0, state), 9, 0); err != nil || got != state {
			t.Fatal(got, err)
		}
	}
}

func TestPrivateAetherPDCaptureRetrieval(t *testing.T) {
	for _, change := range []bool{false, true} {
		t.Run(fmt.Sprint("changed_metadata=", change), func(t *testing.T) {
			p, peer := privateStatusFixture(t)
			done := make(chan error, 1)
			go func() {
				var id uint64
				for {
					request := make([]byte, 32)
					n, err := peer.Read(request)
					if err != nil {
						done <- err
						return
					}
					var offset uint32
					state := uint32(2)
					if n == 17 && request[0] == 'B' {
						id = binary.BigEndian.Uint64(request[9:17])
						state = 1
						if id != binary.BigEndian.Uint64(request[1:9]) {
							done <- fmt.Errorf("begin ID not nonce")
							return
						}
					} else if n == 21 && request[0] == 'R' {
						if binary.BigEndian.Uint64(request[9:17]) != id {
							done <- fmt.Errorf("capture ID changed")
							return
						}
						offset = binary.BigEndian.Uint32(request[17:21])
					} else {
						done <- fmt.Errorf("unexpected request %x", request[:n])
						return
					}
					body := captureReply(id, offset, state)
					if change && offset != 0 {
						binary.BigEndian.PutUint64(body[32:], 7160000)
					}
					_, err = peer.Write(append(request[1:9], body...))
					if err != nil || offset == aetherPDCaptureSamples-aetherPDCaptureChunk || (change && offset != 0) {
						done <- err
						return
					}
				}
			}()
			capture, err := p.readNativeCapture(context.Background())
			if change {
				if err == nil || len(capture.IQ) != 0 {
					t.Fatal("mixed snapshot returned", err)
				}
			} else {
				if err != nil || len(capture.IQ) != 8*aetherPDCaptureSamples || capture.ID == 0 {
					t.Fatal(len(capture.IQ), err)
				}
				for i := 0; i < len(capture.IQ); i += 8 {
					if binary.LittleEndian.Uint32(capture.IQ[i:]) != 0x3f000000 || binary.LittleEndian.Uint32(capture.IQ[i+4:]) != 0xbe800000 {
						t.Fatal("raw sample changed")
					}
				}
			}
			if err := <-done; err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestPrivateAetherPDCaptureCancellation(t *testing.T) {
	p, peer := privateStatusFixture(t)
	go func() {
		request := make([]byte, 17)
		if _, err := peer.Read(request); err == nil {
			id := binary.BigEndian.Uint64(request[9:])
			_, _ = peer.Write(append(request[1:9], captureReply(id, 0, 1)...))
		}
	}()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Millisecond)
	defer cancel()
	c, err := p.readNativeCapture(ctx)
	if err == nil || len(c.IQ) != 0 {
		t.Fatal("cancelled snapshot returned", err)
	}
}

func TestPDMeasurementCaptureV2(t *testing.T) {
	legacy := captureReply(9, 0, 2)
	v2 := append(append(append([]byte{}, legacy[:72]...), make([]byte, 8)...), legacy[72:]...)
	copy(v2, "PDIQ0002")
	binary.BigEndian.PutUint32(v2[68:], 7)
	binary.BigEndian.PutUint64(v2[72:], 31)
	parsed, state, err := parseAetherPDCapture(v2, 9, 0)
	if err != nil || state != 2 || parsed.Measurement == nil || parsed.Measurement.SourceID != "000000000000001f" || !parsed.Measurement.Tones || parsed.Flags != 3 {
		t.Fatal(parsed, state, err)
	}
	for _, flags := range []uint32{4, 6, 8} {
		bad := append([]byte{}, v2...)
		binary.BigEndian.PutUint32(bad[68:], flags)
		if _, _, err := parseAetherPDCapture(bad, 9, 0); err == nil {
			t.Fatal("invalid v2 source flags accepted", flags)
		}
	}
	binary.BigEndian.PutUint64(v2[72:], 0)
	if _, _, err := parseAetherPDCapture(v2, 9, 0); err == nil {
		t.Fatal("anonymous tone source accepted")
	}
	old, _, err := parseAetherPDCapture(legacy, 9, 0)
	if err != nil || old.Measurement != nil {
		t.Fatal("legacy capture gained source provenance")
	}
}
