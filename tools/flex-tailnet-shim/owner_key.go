package main

import (
	"encoding/hex"
	"errors"
	"io"
	"net/http"
	"strings"
)

var errOwnerKeyConflict = errors.New("owner-key replacement requires the matching managed installation and no active installer")

type ownerKeyRequest struct {
	InstallationID string `json:"installation_id"`
	NewToken       string `json:"new_token"`
}

type ownerKeyReceipt struct {
	State          string `json:"state"`
	InstallationID string `json:"installation_id"`
	TokenSHA256    string `json:"owner_key_sha256"`
}

func ownerKeyHex(value string) bool {
	if len(value) != 64 || strings.ToLower(value) != value {
		return false
	}
	raw, err := hex.DecodeString(value)
	if err != nil {
		return false
	}
	var nonzero byte
	for _, b := range raw {
		nonzero |= b
	}
	clear(raw)
	return nonzero != 0
}

func decodeOwnerKey(r io.Reader) (ownerKeyRequest, error) {
	var req ownerKeyRequest
	raw, err := io.ReadAll(io.LimitReader(r, 513))
	defer clear(raw)
	if err != nil || len(raw) > 512 {
		return req, errors.New("invalid owner-key request size")
	}
	if err = decodePDExactObject(raw, map[string]any{
		"installation_id": &req.InstallationID, "new_token": &req.NewToken,
	}); err != nil || !ownerKeyHex(req.InstallationID) || !ownerKeyHex(req.NewToken) {
		return ownerKeyRequest{}, errors.New("invalid owner-key request")
	}
	return req, nil
}

// The client persists its replacement before requesting this operation. No
// secret is generated in a reply that could be lost. A retry authenticated by
// the current replacement confirms it without another write or retirement.
func (n *Node) replaceOwnerKey(presented string, req ownerKeyRequest) (ownerKeyReceipt, error) {
	var receipt ownerKeyReceipt
	if !ownerKeyHex(req.InstallationID) || !ownerKeyHex(req.NewToken) {
		return receipt, errOwnerKeyConflict
	}
	admitLocked := func() error {
		if !n.checkTokenLocked(presented) {
			return errUnauthorized
		}
		if n.RunDir == "" || n.cfg.InstallationID != req.InstallationID || n.pdInstallActiveLocked() {
			return errOwnerKeyConflict
		}
		return nil
	}
	// Refuse wrong targets and active installers before interrupting boot.
	n.mu.Lock()
	err := admitLocked()
	n.mu.Unlock()
	if err != nil {
		return receipt, err
	}
	end, err := n.beginOp(presented, false)
	if err != nil {
		return receipt, err
	}
	defer end()
	n.configMu.Lock()
	defer n.configMu.Unlock()
	n.mu.Lock()
	if err = admitLocked(); err != nil {
		n.mu.Unlock()
		return receipt, err
	}
	hash := sha256Hex(req.NewToken)
	receipt = ownerKeyReceipt{State: "owner_key_current", InstallationID: req.InstallationID, TokenSHA256: hash}
	if n.cfg.TokenSHA256 == hash {
		n.mu.Unlock()
		return receipt, nil
	}
	cfg := n.cfg
	cfg.TokenSHA256 = hash
	n.ownerKeyChanging = true
	n.mu.Unlock()
	defer func() {
		n.mu.Lock()
		n.ownerKeyChanging = false
		n.mu.Unlock()
	}()
	if err = n.saveConfigLocked(cfg); err != nil {
		return ownerKeyReceipt{}, err
	}
	n.mu.Lock()
	// Only the owner field changes; retain sign-in completion observed during
	// I/O. configMu serializes any corresponding persistent publication.
	n.cfg.TokenSHA256 = hash
	n.retirePDSessionLocked()
	if n.pdProcess != nil {
		n.pdProcess.Stop()
	}
	n.mu.Unlock()
	return receipt, nil
}

func ownerKeyHandler(n *Node) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		token := bearer(r)
		if !n.checkToken(token) {
			writeJSON(w, http.StatusUnauthorized, apiError{errUnauthorized.Error()})
			return
		}
		if r.URL.RawQuery != "" || r.URL.ForceQuery {
			writeJSON(w, http.StatusBadRequest, apiError{"owner-key replacement accepts only its fixed identity body"})
			return
		}
		req, err := decodeOwnerKey(r.Body)
		if err != nil {
			writeJSON(w, http.StatusBadRequest, apiError{"malformed owner-key request"})
			return
		}
		receipt, err := n.replaceOwnerKey(token, req)
		switch {
		case errors.Is(err, errUnauthorized):
			writeJSON(w, http.StatusUnauthorized, apiError{errUnauthorized.Error()})
		case errors.Is(err, errOwnerKeyConflict), errors.Is(err, errBusy):
			writeJSON(w, http.StatusConflict, apiError{"owner-key replacement unavailable for this installation or operation"})
		case err != nil:
			// Do not disclose internal paths, and do not claim rollback: native
			// persistence can fail after the replacement was selected.
			writeJSON(w, http.StatusServiceUnavailable, apiError{"owner-key replacement could not be confirmed; retain both credentials"})
		default:
			writeJSON(w, http.StatusOK, receipt)
		}
	}
}
