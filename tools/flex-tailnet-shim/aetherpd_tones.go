package main

import (
	"bytes"
	"context"
	"errors"
	"io"
	"net/http"
	"strconv"
	"time"
)

const (
	pdToneMinSpacing     = 200
	pdToneMaxSpacing     = 10000
	pdToneDefaultSpacing = 1190
	pdToneCenter         = 1305
)

// The source ID and spacing describe one immutable emitted source. Frequencies
// are carrier offsets: center +/- spacing/2, including half-Hz, DC and negative
// offsets. This is neither an RF frequency nor permission to key the radio.
type pdMeasurementToneConfig struct {
	MinSpacingHz uint32 `json:"min_spacing_hz"`
	MaxSpacingHz uint32 `json:"max_spacing_hz"`
	CenterHz     uint32 `json:"center_hz"`
	SpacingHz    uint32 `json:"spacing_hz"`
	SourceID     string `json:"source_id"`
}

func pdToneSpacingValid(spacing uint32) bool {
	return spacing >= pdToneMinSpacing && spacing <= pdToneMaxSpacing
}

func (c *pdMeasurementToneConfig) valid() bool {
	if c == nil || c.MinSpacingHz != pdToneMinSpacing || c.MaxSpacingHz != pdToneMaxSpacing ||
		c.CenterHz != pdToneCenter || !pdToneSpacingValid(c.SpacingHz) || len(c.SourceID) != 16 {
		return false
	}
	id, err := strconv.ParseUint(c.SourceID, 16, 64)
	if err != nil {
		return false
	}
	text := strconv.FormatUint(id, 16)
	return c.SourceID == string(bytes.Repeat([]byte{'0'}, 16-len(text)))+text
}

func pdToneConfig(sourceID string, spacing uint32) *pdMeasurementToneConfig {
	return &pdMeasurementToneConfig{pdToneMinSpacing, pdToneMaxSpacing, pdToneCenter, spacing, sourceID}
}

func parsePDToneConfig(raw []byte) (*pdMeasurementToneConfig, error) {
	var c pdMeasurementToneConfig
	err := decodePDExactObject(raw, map[string]any{
		"min_spacing_hz": &c.MinSpacingHz, "max_spacing_hz": &c.MaxSpacingHz,
		"center_hz": &c.CenterHz, "spacing_hz": &c.SpacingHz, "source_id": &c.SourceID,
	})
	if err != nil || !c.valid() {
		return nil, errors.New("invalid tone configuration")
	}
	return &c, nil
}

func aetherPDToneArmHandler(n *Node) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		token := bearer(r)
		n.mu.Lock()
		authorized := n.checkTokenLocked(token)
		channel, process := n.pdSession, n.pdProcess
		usable := n.pdStarting == nil && !n.pdSessionStopped
		n.mu.Unlock()
		if !authorized {
			writeJSON(w, 401, apiError{errUnauthorized.Error()})
			return
		}
		var spacing uint32
		body, err := io.ReadAll(io.LimitReader(r.Body, 129))
		if err != nil || len(body) > 128 || r.URL.RawQuery != "" ||
			decodePDExactObject(body, map[string]any{"spacing_hz": &spacing}) != nil || !pdToneSpacingValid(spacing) {
			writeJSON(w, 400, apiError{"expected integer spacing_hz from 200 through 10000"})
			return
		}
		if !usable {
			writeJSON(w, 503, map[string]string{"state": "measurement_unavailable"})
			return
		}
		if channel == nil {
			if process == nil {
				writeJSON(w, 409, apiError{errAetherPDNotManaged.Error()})
				return
			}
			request := r.Clone(r.Context())
			request.Body, request.ContentLength = http.NoBody, 0
			aetherPDCalibrationHandlerForProcess(n, 'Q', spacing, observePDReceiveWorld, process)(w, request)
			return
		}
		if !channel.toneSupported {
			writeJSON(w, 501, apiError{"configured measurement is not supported by this session"})
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 8*time.Second)
		defer cancel()
		reply := channel.CallTone(ctx, spacing)
		n.mu.Lock()
		authorized = n.checkTokenLocked(token)
		current := n.pdSession == channel && n.pdProcess == process && n.pdStarting == nil && !n.pdSessionStopped
		n.mu.Unlock()
		if !authorized || !current || ctx.Err() != nil || reply.err != nil || !channel.alive() {
			channel.Close() // An abandoned arm cannot survive its original owner.
			if !authorized {
				writeJSON(w, 401, apiError{errUnauthorized.Error()})
				return
			}
			writeJSON(w, 503, map[string]string{"state": "measurement_unavailable"})
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(reply.code)
		_, _ = w.Write(reply.body)
	}
}
