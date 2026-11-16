package main

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"time"
)

// Identity is immutable for a native session. Valid means matching radio
// readback; it is not calibration, correction or RF authorization.
type pdFeedbackBinding struct {
	Transport string `json:"transport"`
	Channel   uint32 `json:"channel"`
	Pan       uint32 `json:"pan"`
	GUIHandle uint32 `json:"gui_handle"`
	Valid     bool   `json:"valid"`
	Failed    bool   `json:"failed"`
}

func parsePDFeedbackBinding(raw []byte) (*pdFeedbackBinding, error) {
	var b pdFeedbackBinding
	err := decodePDExactObject(raw, map[string]any{
		"transport": &b.Transport, "channel": &b.Channel, "pan": &b.Pan,
		"gui_handle": &b.GUIHandle, "valid": &b.Valid, "failed": &b.Failed,
	})
	if err != nil || b.Transport != "local_dax_iq" || b.Channel < 1 || b.Channel > 4 || b.Pan == 0 || b.GUIHandle == 0 || (b.Valid && b.Failed) {
		return nil, errors.New("invalid IQ binding")
	}
	return &b, nil
}
func selectedPDBindingStatus(reply pdSessionReply, id pdStartIdentity) (bool, error) {
	if reply.err != nil {
		return false, reply.err
	}
	if reply.code != http.StatusOK || len(reply.body) == 0 || len(reply.body) > pdSessionMaxBody {
		return false, errors.New("IQ binding status unavailable")
	}
	fields, err := decodePDFields(reply.body)
	if err != nil {
		return false, err
	}
	var available bool
	if json.Unmarshal(fields["available"], &available) != nil || !available {
		return false, errors.New("native session unavailable")
	}
	binding, err := parsePDFeedbackBinding(fields["feedback_binding"])
	if err != nil {
		return false, err
	}
	if binding.Channel != id.IQChannel || binding.Pan != id.FeedbackPan || binding.GUIHandle != id.GUIHandle || binding.Failed {
		return false, errors.New("native IQ binding does not match request")
	}
	return binding.Valid, nil
}
func confirmPDSelectedBinding(ctx context.Context, channel *pdSessionChannel, id pdStartIdentity) error {
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()
	for {
		ready, err := selectedPDBindingStatus(channel.Call(ctx, 'S'), id)
		if err != nil {
			return err
		}
		if ready {
			return nil
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
		}
	}
}
