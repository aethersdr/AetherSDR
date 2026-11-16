package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
)

// Callers bound the complete body before decoding. Duplicate keys and trailing
// data are rejected even when the caller permits unknown fields.
func decodePDFields(data []byte) (map[string]json.RawMessage, error) {
	dec := json.NewDecoder(bytes.NewReader(data))
	token, err := dec.Token()
	if err != nil || token != json.Delim('{') {
		return nil, errors.New("expected status object")
	}
	fields := make(map[string]json.RawMessage)
	for dec.More() {
		token, err = dec.Token()
		if err != nil {
			return nil, err
		}
		key, ok := token.(string)
		if !ok {
			return nil, errors.New("invalid status key")
		}
		if _, exists := fields[key]; exists {
			return nil, errors.New("duplicate status key")
		}
		var value json.RawMessage
		if err = dec.Decode(&value); err != nil {
			return nil, err
		}
		fields[key] = value
	}
	if _, err = dec.Token(); err != nil {
		return nil, err
	}
	if _, err = dec.Token(); err != io.EOF {
		return nil, errors.New("trailing status data")
	}
	return fields, nil
}
func decodePDExactObject(data []byte, destinations map[string]any) error {
	fields, err := decodePDFields(data)
	if err != nil {
		return err
	}
	if len(fields) != len(destinations) {
		return errors.New("invalid fixed object fields")
	}
	for name, dst := range destinations {
		raw, ok := fields[name]
		if !ok || bytes.Equal(raw, []byte("null")) {
			return errors.New("missing fixed object field")
		}
		if err = json.Unmarshal(raw, dst); err != nil {
			return err
		}
	}
	return nil
}
