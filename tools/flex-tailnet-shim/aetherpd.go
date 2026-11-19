package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"time"
)

const aetherPDAddress = "127.0.0.1:48996"
const aetherPDMaxStatusBytes = 16 << 10

var aetherPDReads = make(chan struct{}, 1)

// Operational state, selected IQ binding and bounded failure records cross the
// shim boundary. Native ownership hints remain private.
// This report never authorizes capture provisioning or transmitter control.
type aetherPDStatus struct {
	nativeActiveSlice             *int
	CaptureBatchVersion           uint32                   `json:"measurement_capture_batch_version,omitempty"`
	Diagnostics                   *pdDiagnostics           `json:"diagnostics,omitempty"`
	FeedbackBinding               *pdFeedbackBinding       `json:"feedback_binding,omitempty"`
	MeasurementToneCaptureVersion uint32                   `json:"measurement_tone_capture_version,omitempty"`
	MeasurementToneControl        bool                     `json:"measurement_tone_control,omitempty"`
	MeasurementToneConfig         *pdMeasurementToneConfig `json:"measurement_tone_config,omitempty"`
	Measurement                   *pdMeasurementSource     `json:"measurement,omitempty"`
	InputLimitedSamples           uint64                   `json:"input_limited_samples"`
	CorrectionControl             bool                     `json:"correction_control,omitempty"`
	MeasurementControl            bool                     `json:"measurement_control,omitempty"`
	SourcePurpose                 string                   `json:"source_purpose,omitempty"`
	CalibrationControl            bool                     `json:"calibration_control"`
	CalibrationState              string                   `json:"calibration_state,omitempty"`
	CalibrationSamplesLeft        uint32                   `json:"calibration_samples_left,omitempty"`
	Available                     bool                     `json:"available"`
	State                         string                   `json:"state"`
	Registered                    bool                     `json:"registered"`
	Keyed                         bool                     `json:"keyed"`
	CorrectionEnabled             bool                     `json:"correction_enabled"`
	Correcting                    bool                     `json:"correcting"`
	CalibrationRequired           bool                     `json:"calibration_required"`
	Faults                        aetherPDFaults           `json:"faults"`
}

type aetherPDFaults struct {
	DispatchDeadlines uint64 `json:"dispatch_deadlines"`
	SendErrors        uint64 `json:"send_errors"`
	RejectedInput     uint64 `json:"rejected_input"`
	StaleIQ           uint64 `json:"stale_iq"`
}

func parseAetherPDStatus(data []byte) (aetherPDStatus, error) {
	var result aetherPDStatus
	if len(data) == 0 || len(data) > aetherPDMaxStatusBytes {
		return result, errors.New("invalid status size")
	}
	fields, err := decodePDFields(data)
	if err != nil {
		return result, err
	}
	read := func(name string, dst any) error {
		value, ok := fields[name]
		if !ok || bytes.Equal(value, []byte("null")) {
			return fmt.Errorf("missing %s", name)
		}
		return json.Unmarshal(value, dst)
	}
	var name string
	if err = read("name", &name); err != nil || name != "AetherPD" {
		return result, errors.New("unexpected status producer")
	}
	if _, present := fields["active_slice"]; present {
		var slice int
		if err = read("active_slice", &slice); err != nil || slice < -1 || slice >= 16 {
			return result, errors.New("invalid native active slice")
		}
		result.nativeActiveSlice = &slice
	}
	var fault, blocked bool
	for _, field := range []struct {
		name string
		dst  any
	}{
		{"registered", &result.Registered}, {"keyed", &result.Keyed}, {"correction", &result.CorrectionEnabled},
		{"correcting", &result.Correcting}, {"local_iq_fault", &fault}, {"native_apd_blocked", &blocked},
		{"tx_deadline_faults", &result.Faults.DispatchDeadlines}, {"tx_send_errors", &result.Faults.SendErrors},
		{"tx_rejected", &result.Faults.RejectedInput}, {"local_iq_stale", &result.Faults.StaleIQ},
	} {
		if err = read(field.name, field.dst); err != nil {
			return result, err
		}
	}
	if _, ok := fields["calibration_control"]; ok {
		for _, field := range []struct {
			name string
			dst  any
		}{{"calibration_control", &result.CalibrationControl}, {"calibration_state", &result.CalibrationState}, {"calibration_samples_left", &result.CalibrationSamplesLeft}} {
			if err = read(field.name, field.dst); err != nil {
				return result, err
			}
		}
		switch result.CalibrationState {
		case "idle", "completed", "canceled", "expired":
			if result.CalibrationSamplesLeft != 0 {
				return result, errors.New("inactive calibration has samples remaining")
			}
		case "armed", "running":
			if !result.CalibrationControl || result.CalibrationSamplesLeft == 0 || result.CalibrationSamplesLeft > 192000 || (result.CalibrationState == "running") != result.Keyed {
				return result, errors.New("inconsistent calibration state")
			}
		default:
			return result, errors.New("invalid calibration state")
		}
	} else if _, ok = fields["calibration_state"]; ok {
		return result, errors.New("partial calibration status")
	} else if _, present := fields["calibration_samples_left"]; present {
		return result, errors.New("partial calibration status")
	}
	if _, present := fields["correction_control"]; present {
		if err = read("correction_control", &result.CorrectionControl); err != nil || (result.CorrectionControl && !result.CalibrationControl) {
			return result, errors.New("invalid correction control capability")
		}
	}
	if _, present := fields["measurement_control"]; present {
		if err = read("measurement_control", &result.MeasurementControl); err != nil {
			return result, err
		}
		if err = read("source_purpose", &result.SourcePurpose); err != nil {
			return result, err
		}
		if result.SourcePurpose != "calibration" && result.SourcePurpose != "measurement" {
			return result, errors.New("invalid source purpose")
		}
		if result.MeasurementControl && !result.CalibrationControl {
			return result, errors.New("measurement requires source control")
		}
		if result.SourcePurpose == "measurement" && !result.MeasurementControl {
			return result, errors.New("measurement source unavailable")
		}
	} else if _, present := fields["source_purpose"]; present {
		return result, errors.New("partial measurement status")
	}
	if raw, present := fields["measurement"]; present {
		result.Measurement, err = parsePDMeasurementSource(raw)
		if err != nil {
			return result, err
		}
		active := result.Measurement.SourceID != "0000000000000000"
		if active && (!result.MeasurementControl || result.SourcePurpose != "measurement") {
			return result, errors.New("measurement identity without capability")
		}
		if result.Measurement.Tones && (!result.Keyed || result.CalibrationState != "running") {
			return result, errors.New("measurement tones outside running source")
		}
	}
	if raw, present := fields["measurement_tone_config"]; present {
		result.MeasurementToneConfig, err = parsePDToneConfig(raw)
		if err != nil {
			return result, err
		}
		if err = read("measurement_tone_capture_version", &result.MeasurementToneCaptureVersion); err != nil || result.MeasurementToneCaptureVersion != 3 {
			return result, errors.New("measurement tones require snapshot-bound configuration")
		}
		if err = read("measurement_tone_control", &result.MeasurementToneControl); err != nil ||
			(result.MeasurementToneControl && !result.MeasurementControl) || result.Measurement == nil ||
			result.Measurement.SourceID != result.MeasurementToneConfig.SourceID {
			return result, errors.New("inconsistent measurement tone configuration")
		}
	} else if _, present := fields["measurement_tone_control"]; present {
		return result, errors.New("partial measurement tone configuration")
	} else if _, present := fields["measurement_tone_capture_version"]; present {
		return result, errors.New("partial measurement tone capture capability")
	}
	if _, present := fields["measurement_capture_batch_version"]; present {
		if err = read("measurement_capture_batch_version", &result.CaptureBatchVersion); err != nil || result.CaptureBatchVersion != 1 || result.MeasurementToneConfig == nil {
			return result, errors.New("invalid measurement batch capability")
		}
	}
	if _, present := fields["limited_samples"]; present {
		if err = read("limited_samples", &result.InputLimitedSamples); err != nil {
			return result, err
		}
	}
	if raw, present := fields["feedback_binding"]; present {
		result.FeedbackBinding, err = parsePDFeedbackBinding(raw)
		if err != nil {
			return result, err
		}
		if !result.FeedbackBinding.Valid && (result.Correcting || result.CalibrationState == "armed" || result.CalibrationState == "running") {
			return result, errors.New("active source without IQ binding")
		}
	}
	if raw, present := fields["diagnostics"]; present {
		result.Diagnostics, err = parsePDDiagnostics(raw)
		if err != nil {
			return result, err
		}
	}
	var transportPending bool
	if _, present := fields["transport_pending"]; present {
		if err = read("transport_pending", &transportPending); err != nil {
			return result, err
		}
		if transportPending {
			// Pending is authoritative; an enable flag, when present, must agree.
			if _, enabledPresent := fields["transport_cache_enabled"]; enabledPresent {
				var cacheEnabled bool
				if err = read("transport_cache_enabled", &cacheEnabled); err != nil || !cacheEnabled {
					return result, errors.New("transport recovery is not enabled")
				}
			}
			if !fault || blocked || !result.Registered || !result.Keyed || !result.CorrectionEnabled || result.Correcting || result.Faults.DispatchDeadlines == 0 {
				return result, errors.New("inconsistent transport recovery state")
			}
		}
	}
	if result.Correcting && (!result.Registered || !result.Keyed || !result.CorrectionEnabled || fault || blocked) {
		return aetherPDStatus{}, errors.New("inconsistent correction state")
	}
	result.Available = true
	result.CalibrationRequired = fault && !transportPending && (result.Faults.DispatchDeadlines > 0 || result.Faults.SendErrors > 0 || result.Faults.RejectedInput > 0 || result.Faults.StaleIQ > 0)
	switch {
	case !result.Registered:
		result.State = "starting"
	case result.FeedbackBinding != nil && result.FeedbackBinding.Failed:
		result.State = "feedback_binding_failed"
	case result.FeedbackBinding != nil && !result.FeedbackBinding.Valid:
		result.State = "feedback_binding_pending"
	case !result.CorrectionEnabled:
		result.State = "disabled"
	case blocked:
		result.State = "blocked_by_radio_apd"
	case result.CalibrationState == "armed":
		result.State = "calibration_armed"
		if result.SourcePurpose == "measurement" {
			result.State = "measurement_armed"
		}
	case result.CalibrationRequired:
		result.State = "recalibration_required"
	case !result.Keyed:
		result.State = "idle"
	case result.Correcting:
		result.State = "correcting"
	default:
		result.State = "calibrating"
	}
	return result, nil
}

// The native service sends one JSON object then closes. It receives no bytes.
func queryAetherPD(ctx context.Context) (aetherPDStatus, error) {
	return queryAetherPDWithDial(ctx, (&net.Dialer{}).DialContext)
}

func queryAetherPDWithDial(ctx context.Context, dial func(context.Context, string, string) (net.Conn, error)) (aetherPDStatus, error) {
	ctx, cancel := context.WithTimeout(ctx, time.Second)
	defer cancel()
	select {
	case aetherPDReads <- struct{}{}:
		defer func() { <-aetherPDReads }()
	case <-ctx.Done():
		return aetherPDStatus{}, ctx.Err()
	}
	conn, err := dial(ctx, "tcp4", aetherPDAddress)
	if err != nil {
		return aetherPDStatus{}, err
	}
	defer conn.Close()
	stop := context.AfterFunc(ctx, func() { _ = conn.Close() })
	defer stop()
	deadline, _ := ctx.Deadline()
	if err = conn.SetReadDeadline(deadline); err != nil {
		return aetherPDStatus{}, err
	}
	data, err := io.ReadAll(io.LimitReader(conn, aetherPDMaxStatusBytes+1))
	if err != nil {
		return aetherPDStatus{}, err
	}
	return parseAetherPDStatus(data)
}

func aetherPDStatusHandler(n *Node, read func(context.Context) (aetherPDStatus, error)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		token := bearer(r)
		if !n.checkToken(token) {
			time.Sleep(500 * time.Millisecond)
			writeJSON(w, http.StatusUnauthorized, apiError{errUnauthorized.Error()})
			return
		}
		if r.URL.RawQuery != "" || r.ContentLength != 0 {
			writeJSON(w, http.StatusBadRequest, apiError{"this endpoint accepts no parameters or body"})
			return
		}
		result, err := read(r.Context())
		// A token revoked while the local read was pending cannot retrieve its result.
		if !n.checkToken(token) {
			writeJSON(w, http.StatusUnauthorized, apiError{errUnauthorized.Error()})
			return
		}
		if err != nil {
			writeJSON(w, http.StatusServiceUnavailable, struct {
				Available bool   `json:"available"`
				State     string `json:"state"`
			}{false, "unavailable"})
			return
		}
		if !result.Available && (result.State == "stopped" || result.State == "cleanup_unconfirmed" || result.State == "stopped_receive_unconfirmed") {
			// Supervisor completion carries no current RF state or DSP counters.
			writeJSON(w, http.StatusOK, struct {
				Available bool   `json:"available"`
				State     string `json:"state"`
			}{false, result.State})
			return
		}
		writeJSON(w, http.StatusOK, result)
	}
}

var errAetherPDNotManaged = errors.New("AetherPD is not owned by this shim")

func (n *Node) stopAetherPD(presented string) error {
	n.mu.Lock()
	defer n.mu.Unlock()
	if !n.checkTokenLocked(presented) {
		return errUnauthorized
	}
	if n.pdProcess == nil {
		return errAetherPDNotManaged
	}
	n.pdProcess.Stop()
	return nil
}
