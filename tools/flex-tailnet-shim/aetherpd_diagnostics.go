package main

import (
	"encoding/json"
	"errors"
	"strings"
)

// These are historical observations, never inputs to admission or recovery.
// Each counter and latest record belongs only to the current DSP process.
type pdDiagnosticRecord struct {
	Count     uint32 `json:"count"`
	Truncated bool   `json:"truncated"`
	Text      string `json:"text"`
}

type pdDiagnostics struct {
	Version   uint32             `json:"version"`
	Alignment pdDiagnosticRecord `json:"alignment"`
	SourceEnd pdDiagnosticRecord `json:"source_end"`
}

func parsePDDiagnosticRecord(raw []byte, prefix string) (pdDiagnosticRecord, error) {
	var r pdDiagnosticRecord
	err := decodePDExactObject(raw, map[string]any{"count": &r.Count, "truncated": &r.Truncated, "text": &r.Text})
	if err != nil || len(r.Text) > 1023 || (r.Count == 0 && (r.Text != "" || r.Truncated)) ||
		(r.Count != 0 && !strings.HasPrefix(r.Text, prefix)) || (r.Truncated && len(r.Text) != 1023) {
		return r, errors.New("invalid diagnostic record")
	}
	for _, ch := range []byte(r.Text) {
		if ch < 32 || ch > 126 || ch == '"' || ch == '\\' {
			return r, errors.New("invalid diagnostic text")
		}
	}
	return r, nil
}

func parsePDDiagnostics(raw []byte) (*pdDiagnostics, error) {
	var d pdDiagnostics
	var alignment, source json.RawMessage
	err := decodePDExactObject(raw, map[string]any{"version": &d.Version, "alignment": &alignment, "source_end": &source})
	if err != nil || d.Version != 1 {
		return nil, errors.New("invalid diagnostic schema")
	}
	if d.Alignment, err = parsePDDiagnosticRecord(alignment, "alignment evidence: reason="); err != nil {
		return nil, err
	}
	if d.SourceEnd, err = parsePDDiagnosticRecord(source, "bounded source ended: reason="); err != nil {
		return nil, err
	}
	return &d, nil
}
