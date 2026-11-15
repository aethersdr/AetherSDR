package main

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"sort"
	"strconv"
	"strings"
	"time"
)

const pdReceiveSubscriptions = "C1|sub client all\nC2|sub pan all\nC3|sub slice all\nC4|sub tx all\n"

// Observation is deliberately not a recovery admission: resource capacity and
// the full receive-profile coverage must still be established before writes.
type pdReceiveObservation struct {
	original          pdReceiveOriginal
	view              pdReceiveView
	interlockComplete bool
}

type pdReceiveAttributes map[string]string

func pdReceiveAttributesFrom(words []string) (pdReceiveAttributes, error) {
	attrs := make(pdReceiveAttributes, len(words))
	for _, word := range words {
		key, value, ok := strings.Cut(word, "=")
		if !ok || key == "" {
			return nil, errors.New("invalid observation attribute")
		}
		if _, duplicate := attrs[key]; duplicate {
			return nil, errors.New("duplicate observation attribute")
		}
		attrs[key] = value
	}
	return attrs, nil
}

func (attrs pdReceiveAttributes) merge(delta pdReceiveAttributes) {
	for key, value := range delta {
		attrs[key] = value
	}
}

func (attrs pdReceiveAttributes) sliceLine(id int) string {
	keys := make([]string, 0, len(attrs))
	for key := range attrs {
		keys = append(keys, key)
	}
	sort.Strings(keys)
	var line strings.Builder
	fmt.Fprintf(&line, "S0|slice %d", id)
	for _, key := range keys {
		fmt.Fprintf(&line, " %s=%s", key, attrs[key])
	}
	return line.String()
}

// The caller gets a fresh connection, never a cached READY from the PD lease.
// Subscriptions identify the original GUI before binding this observer to it.
// Firmware 4.3.16 gives unbound clients a separate, non-ready TX context.
func observePDReceive(ctx context.Context, guiHandle, panID uint32, sliceID int) (pdReceiveObservation, error) {
	return observePDReceiveState(ctx, guiHandle, panID, sliceID, true)
}

// Recovery/journaling needs current ownership even while the original slice is
// in PD or absent. Never interpret that state as new original receive settings.
func observePDReceiveWorld(ctx context.Context, original pdReceiveOriginal) (pdReceiveView, error) {
	observation, err := observePDReceiveState(ctx, original.guiHandle, original.panID, original.sliceID, false)
	return observation.view, err
}

func observePDReceiveState(ctx context.Context, guiHandle, panID uint32, sliceID int, captureOriginal bool) (pdReceiveObservation, error) {
	ctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	var dialer net.Dialer
	conn, err := dialer.DialContext(ctx, "tcp4", "127.0.0.1:4992")
	if err != nil {
		return pdReceiveObservation{}, err
	}
	defer conn.Close()
	deadline, _ := ctx.Deadline()
	if err := conn.SetDeadline(deadline); err != nil {
		return pdReceiveObservation{}, err
	}
	observation, err := readPDReceiveState(conn, guiHandle, panID, sliceID, time.Now, captureOriginal)
	if ctx.Err() != nil {
		return pdReceiveObservation{}, ctx.Err()
	}
	return observation, err
}

// Bounded synchronous reader; the transport must enforce the overall deadline.
// Subscription replies cover the inventory, but the live interlock broadcast
// can arrive later (observed up to 400 ms on firmware 4.3.16). Wait for both.
func readPDReceiveObservation(conn io.ReadWriter, guiHandle, panID uint32, sliceID int, now func() time.Time) (pdReceiveObservation, error) {
	return readPDReceiveState(conn, guiHandle, panID, sliceID, now, true)
}

func readPDReceiveState(conn io.ReadWriter, guiHandle, panID uint32, sliceID int, now func() time.Time, captureOriginal bool) (pdReceiveObservation, error) {
	var out pdReceiveObservation
	autoSelect := panID == 0 && sliceID == -1
	if guiHandle == 0 || !autoSelect && (panID == 0 || sliceID < 0 || sliceID >= 16) {
		return out, errors.New("invalid prepared receive identity")
	}
	if n, err := io.WriteString(conn, pdReceiveSubscriptions); err != nil {
		return out, err
	} else if n != len(pdReceiveSubscriptions) {
		return out, io.ErrShortWrite
	}
	reader := bufio.NewReaderSize(io.LimitReader(conn, 1<<20), 4098)
	var replies [4]bool
	bindSent, bound := false, false
	var slices [16]pdReceiveAttributes
	pans := make(map[uint32]pdReceiveAttributes)
	interlock := make(pdReceiveAttributes)
	clientID := ""
	connected := false
	remainingSlices := -1
	var firstObservation time.Time
	for count := 0; count < 8192; count++ {
		line, err := reader.ReadSlice('\n')
		if err != nil {
			return out, fmt.Errorf("incomplete receive observation: %w", err)
		}
		line = line[:len(line)-1]
		if len(line) > 0 && line[len(line)-1] == '\r' {
			line = line[:len(line)-1]
		}
		if len(line) == 0 || len(line) > 4096 {
			return out, errors.New("invalid observation line size")
		}
		for _, c := range line {
			// FlexLib uses DEL to encode spaces and separate installed waveform
			// metadata. Required identity/numeric fields are validated below.
			if c < 32 || c > 127 {
				return out, errors.New("invalid observation character")
			}
		}
		text := string(line)
		if line[0] == 'R' {
			fields := strings.SplitN(text[1:], "|", 4)
			if len(fields) < 3 {
				return out, errors.New("invalid subscription reply")
			}
			seq, seqErr := pdReceiveDecimal(fields[0])
			code, codeErr := strconv.ParseUint(fields[1], 16, 32)
			if seqErr != nil || codeErr != nil || seq < 1 || seq > 5 || code != 0 {
				return out, errors.New("failed or unexpected subscription reply")
			}
			if seq == 5 {
				handle, err := pdReceiveHexHandle(fields[2])
				if !bindSent || bound || err != nil || handle != guiHandle {
					return out, errors.New("receive observer GUI bind was not confirmed")
				}
				bound = true
			} else {
				if replies[seq-1] {
					return out, errors.New("duplicate subscription reply")
				}
				replies[seq-1] = true
			}
			if replies == [4]bool{true, true, true, true} && !bindSent {
				if !connected {
					return out, errors.New("missing GUI identity before observer bind")
				}
				command := "C5|client bind client_id=" + clientID + "\n"
				if n, err := io.WriteString(conn, command); err != nil {
					return out, err
				} else if n != len(command) {
					return out, io.ErrShortWrite
				}
				// Bind emits its status before the acknowledgement. Discard only
				// the unbound context; keep subsequent status and require both.
				interlock = make(pdReceiveAttributes)
				bindSent = true
			}
			if bound && pdReceiveInterlockComplete(interlock) {
				break
			}
			continue
		}
		if line[0] != 'S' {
			if line[0] != 'V' && line[0] != 'H' && line[0] != 'M' {
				return out, errors.New("unexpected radio frame")
			}
			continue
		}
		divider := strings.IndexByte(text, '|')
		if divider < 2 || divider > 9 || strings.Count(text, "|") != 1 {
			return out, errors.New("invalid observation status header")
		}
		if _, err := strconv.ParseUint(text[1:divider], 16, 32); err != nil {
			return out, errors.New("invalid observation status recipient")
		}
		words := strings.Fields(text[divider+1:])
		if len(words) == 0 {
			return out, errors.New("empty observation status")
		}
		if firstObservation.IsZero() {
			firstObservation = now()
		}
		var attributes pdReceiveAttributes
		var target pdReceiveAttributes
		switch {
		case words[0] == "radio" && len(words) > 1 && strings.Contains(words[1], "="):
			attrs, err := pdReceiveAttributesFrom(words[1:])
			if err != nil {
				return out, err
			}
			if value, present := attrs["slices"]; present {
				remainingSlices, err = pdReceiveDecimal(value)
				if err != nil || remainingSlices < 0 || remainingSlices > 16 {
					return out, errors.New("invalid remaining slice resources")
				}
			}
			continue
		case words[0] == "client" && len(words) >= 3:
			handle, err := pdReceiveHexHandle(words[1])
			if err != nil {
				return out, err
			}
			if handle != guiHandle {
				continue
			}
			if words[2] != "connected" {
				return out, errors.New("prepared GUI disconnected")
			}
			attributes, err = pdReceiveAttributesFrom(words[3:])
			if err != nil || !pdReceiveClientIDValid(attributes["client_id"]) {
				return out, errors.New("missing GUI identity")
			}
			if connected && clientID != attributes["client_id"] {
				return out, errors.New("GUI identity changed")
			}
			connected, clientID = true, attributes["client_id"]
			continue
		case words[0] == "display" && len(words) >= 4 && words[1] == "pan":
			id, err := pdReceiveHexHandle(words[2])
			if err != nil {
				return out, err
			}
			if !autoSelect && id != panID {
				continue
			}
			if words[3] == "removed" {
				if !autoSelect {
					return out, errors.New("prepared pan removed")
				}
				delete(pans, id)
				continue
			}
			attributes, err = pdReceiveAttributesFrom(words[3:])
			if err != nil {
				return out, err
			}
			if owner, present := attributes["client_handle"]; present {
				parsed, err := pdReceiveHexHandle(owner)
				if err != nil || !autoSelect && parsed != guiHandle {
					return out, errors.New("prepared pan owner changed")
				}
				if previous := pans[id]; previous != nil && previous["client_handle"] != owner {
					delete(pans, id)
				}
			}
			if pans[id] == nil {
				if len(pans) >= 16 {
					return out, errors.New("pan inventory exceeds supported bound")
				}
				pans[id] = make(pdReceiveAttributes)
			}
			target = pans[id]
		case words[0] == "slice" && len(words) >= 3:
			id, err := pdReceiveDecimal(words[1])
			if err != nil || id < 0 || id >= len(slices) {
				return out, errors.New("slice inventory exceeds supported bound")
			}
			if words[2] == "removed" {
				slices[id] = nil
				continue
			}
			attributes, err = pdReceiveAttributesFrom(words[2:])
			if err != nil {
				return out, err
			}
			if attributes["in_use"] == "0" {
				slices[id] = nil
				continue
			}
			if owner, present := attributes["client_handle"]; present && slices[id] != nil && slices[id]["client_handle"] != owner {
				slices[id] = nil
			}
			if slices[id] == nil {
				slices[id] = make(pdReceiveAttributes)
			}
			target = slices[id]
		case words[0] == "interlock" && len(words) >= 2 && strings.Contains(words[1], "="):
			attributes, err = pdReceiveAttributesFrom(words[1:])
			if err != nil {
				return out, err
			}
			target = interlock
		default:
			continue
		}
		target.merge(attributes)
		if bound && pdReceiveInterlockComplete(interlock) {
			break
		}
	}
	if replies != [4]bool{true, true, true, true} || !bound || !connected || firstObservation.IsZero() {
		return out, errors.New("receive observation barrier incomplete")
	}
	if autoSelect {
		for id, attrs := range slices {
			owner, err := pdReceiveHexHandle(attrs["client_handle"])
			if err == nil && owner == guiHandle && attrs["active"] == "1" && attrs["in_use"] == "1" {
				if sliceID != -1 {
					return out, errors.New("ambiguous active GUI receiver")
				}
				sliceID = id
				panID, err = pdReceiveHexHandle(attrs["pan"])
				if err != nil {
					return out, err
				}
			}
		}
		if sliceID == -1 {
			return out, errors.New("no active GUI receiver to capture")
		}
	}
	pan := pans[panID]
	view := pdReceiveView{guiHandle: guiHandle, clientID: clientID, guiConnected: true, panID: panID, observedAt: firstObservation}
	var err error
	if remainingSlices >= 0 {
		view.complete = true
		view.remainingSlices = remainingSlices
		view.maxSlices = 16
		view.maxDAX = 8
	}
	view.panOwner, err = pdReceiveHexHandle(pan["client_handle"])
	if err != nil || view.panOwner != guiHandle {
		return out, errors.New("missing owned pan snapshot")
	}
	view.panGain, err = pdReceiveDecimal(pan["rfgain"])
	if err != nil || view.panGain < -100 || view.panGain > 100 {
		return out, errors.New("invalid pan gain")
	}
	center, centerErr := pdReceiveFrequencyHz(pan["center"])
	bandwidth, bandwidthErr := pdReceiveFrequencyHz(pan["bandwidth"])
	if centerErr != nil || bandwidthErr != nil || bandwidth > 100_000_000 || center <= bandwidth/2 {
		return out, errors.New("invalid pan coverage")
	}
	view.panLowHz, view.panHighHz = center-bandwidth/2, center+bandwidth/2
	view.panAntenna = pan["rxant"]
	interlockComplete := pdReceiveInterlockComplete(interlock)
	if !interlockComplete {
		return out, errors.New("missing fresh live interlock status")
	}
	if interlockComplete {
		ownerText := interlock["tx_client_handle"]
		if ownerText != "0x00000000" {
			view.txOwner, err = pdReceiveHexHandle(ownerText)
			if err != nil {
				return out, err
			}
		}
		if interlock["tx_allowed"] != "0" && interlock["tx_allowed"] != "1" {
			return out, errors.New("invalid transmit permission")
		}
		view.receive = interlock["state"] == "RECEIVE"
		view.ready, view.txAllowed = interlock["state"] == "READY", interlock["tx_allowed"] == "1"
		view.source, view.reason = interlock["source"], interlock["reason"]
	}
	for id, attrs := range slices {
		if attrs == nil {
			continue
		}
		entry := pdReceiveSlice{present: true, mode: attrs["mode"]}
		entry.owner, err = pdReceiveHexHandle(attrs["client_handle"])
		if err != nil {
			return out, errors.New("slice inventory missing owner")
		}
		entry.panID, err = pdReceiveHexHandle(attrs["pan"])
		if err != nil {
			return out, err
		}
		entry.frequencyHz, err = pdReceiveFrequencyHz(attrs["RF_frequency"])
		if err != nil || entry.mode == "" {
			return out, errors.New("incomplete slice inventory")
		}
		entry.daxChannel, err = pdReceiveDecimal(attrs["dax"])
		if err != nil || entry.daxChannel < 0 || entry.daxChannel > 8 {
			return out, errors.New("invalid inventory DAX channel")
		}
		view.slices[id] = entry
	}
	var original pdReceiveOriginal
	if captureOriginal {
		original, err = capturePDReceiveOriginal(slices[sliceID].sliceLine(sliceID), guiHandle, clientID, panID, view.panGain)
		if err != nil {
			return out, err
		}
		if view.panAntenna != original.settings.antenna {
			return out, errors.New("pan and slice antenna mismatch")
		}
	}
	return pdReceiveObservation{original: original, view: view, interlockComplete: interlockComplete}, nil
}

func pdReceiveInterlockComplete(attributes pdReceiveAttributes) bool {
	for _, key := range []string{"state", "tx_allowed", "source", "reason", "tx_client_handle"} {
		if _, present := attributes[key]; !present {
			return false
		}
	}
	return true
}
