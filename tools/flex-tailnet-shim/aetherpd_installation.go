package main

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"net/http"
)

const pdInstallationSize = 204

var errPDInstallationUnavailable = errors.New("installed transaction status unavailable")

type pdInstallation struct {
	State          string `json:"state"`
	Committed      bool   `json:"installation_committed"`
	PlanDigest     string `json:"upgrade_plan_blake2b_256"`
	SelectedDigest string `json:"selected_release_blake2b_256"`
	SelectedKey    string `json:"selected_release_key"`
	Profile        uint32 `json:"profile"`
	Firmware       string `json:"firmware"`
	Serial         string `json:"serial"`
}

func pdInstallationText(data []byte) (string, bool) {
	end := bytes.IndexByte(data, 0)
	if end < 1 {
		return "", false
	}
	for i, value := range data {
		if (i < end && (value < 0x21 || value > 0x7e)) || (i >= end && value != 0) {
			return "", false
		}
	}
	return string(data[:end]), true
}

func parsePDInstallation(data []byte) (pdInstallation, error) {
	var status pdInstallation
	if len(data) != pdInstallationSize || string(data[:8]) != "PDVIEW01" {
		return status, errPDInstallationUnavailable
	}
	phase, profile := binary.BigEndian.Uint32(data[8:12]), binary.BigEndian.Uint32(data[12:16])
	if (phase != 4 && phase != 7) || (profile != 1 && profile != 2) {
		return status, errPDInstallationUnavailable
	}
	for offset := 16; offset < 112; offset += 32 {
		if bytes.Equal(data[offset:offset+32], make([]byte, 32)) {
			return status, errPDInstallationUnavailable
		}
	}
	firmware, firmwareOK := pdInstallationText(data[112:140])
	serial, serialOK := pdInstallationText(data[140:204])
	if !firmwareOK || !serialOK {
		return status, errPDInstallationUnavailable
	}
	status = pdInstallation{
		State: "rolled_back", Committed: phase == 4,
		PlanDigest: hex.EncodeToString(data[16:48]), SelectedDigest: hex.EncodeToString(data[48:80]),
		SelectedKey: hex.EncodeToString(data[80:112]), Profile: profile, Firmware: firmware, Serial: serial,
	}
	if status.Committed {
		status.State = "committed"
	}
	return status, nil
}

func aetherPDInstallationHandler(n *Node, read func() (pdInstallation, error)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cache-Control", "no-store")
		if !n.checkToken(bearer(r)) {
			writeJSON(w, http.StatusUnauthorized, apiError{errUnauthorized.Error()})
			return
		}
		if r.URL.RawQuery != "" || r.ContentLength != 0 {
			writeJSON(w, http.StatusBadRequest, apiError{"installation status accepts no parameters or body"})
			return
		}
		status, err := read()
		if err != nil {
			writeJSON(w, http.StatusServiceUnavailable, map[string]any{
				"state": "unavailable", "installation_committed": false, "error": errPDInstallationUnavailable.Error(),
			})
			return
		}
		writeJSON(w, http.StatusOK, status)
	}
}
