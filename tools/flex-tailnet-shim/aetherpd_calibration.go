package main

import (
	"context"
	"errors"
	"log"
	"net/http"
	"time"
)

// Source control is separate from status and PTT. No caller-supplied waveform,
// RF frequency, gain, duration, PID or executable reaches the owned native child.
func aetherPDCalibrationHandler(n *Node, opcode byte) http.HandlerFunc {
	return aetherPDCalibrationHandlerWithObserver(n, opcode, observePDReceiveWorld)
}

func aetherPDCalibrationHandlerWithObserver(n *Node, opcode byte, observe func(context.Context, pdReceiveOriginal) (pdReceiveView, error)) http.HandlerFunc {
	return aetherPDCalibrationHandlerWithTone(n, opcode, 0, observe)
}

func pdMeasurementOpcode(op byte) bool { return op == 'M' || op == 'Q' }
func pdArmOpcode(op byte) bool         { return op == 'A' || pdMeasurementOpcode(op) }

func aetherPDCalibrationHandlerWithTone(n *Node, opcode byte, spacing uint32, observe func(context.Context, pdReceiveOriginal) (pdReceiveView, error)) http.HandlerFunc {
	return aetherPDCalibrationHandlerForProcess(n, opcode, spacing, observe, nil)
}

func aetherPDCalibrationHandlerForProcess(n *Node, opcode byte, spacing uint32, observe func(context.Context, pdReceiveOriginal) (pdReceiveView, error), expected *aetherPDProcess) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		correction := pdCorrectionOpcode(opcode)
		token := bearer(r)
		n.mu.Lock()
		authorized := n.checkTokenLocked(token)
		process := n.pdProcess
		n.mu.Unlock()
		if !authorized {
			time.Sleep(500 * time.Millisecond)
			writeJSON(w, http.StatusUnauthorized, apiError{errUnauthorized.Error()})
			return
		}
		if r.URL.RawQuery != "" || r.ContentLength != 0 {
			writeJSON(w, http.StatusBadRequest, apiError{"this endpoint accepts no parameters or body"})
			return
		}
		if expected != nil && process != expected {
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"state": "calibration_unavailable"})
			return
		}
		if process == nil {
			writeJSON(w, http.StatusConflict, apiError{errAetherPDNotManaged.Error()})
			return
		}
		operationContext := r.Context()
		if pdArmOpcode(opcode) || correction {
			armContext, finish, err := process.calibrationIntents.beginArm(operationContext)
			if errors.Is(err, errPDCalibrationStopped) {
				writeJSON(w, http.StatusServiceUnavailable, map[string]string{"state": "calibration_unavailable"})
				return
			}
			if err != nil {
				writeJSON(w, http.StatusConflict, map[string]string{"state": "calibration_busy"})
				return
			}
			defer finish()
			operationContext = armContext
		} else if opcode == 'X' {
			process.calibrationIntents.cancelPendingArm()
		}
		if pdMeasurementOpcode(opcode) {
			status, err := process.queryStatus(operationContext)
			if err != nil || !status.Available || !status.Registered || !status.MeasurementControl ||
				(opcode == 'Q' && (!status.MeasurementToneControl || !pdToneSpacingValid(spacing))) {
				writeJSON(w, http.StatusServiceUnavailable, map[string]string{"state": "measurement_unavailable"})
				return
			}
		}
		if correction {
			status, err := process.queryStatus(operationContext)
			if err != nil || !status.Available || !status.Registered || !status.CorrectionControl {
				writeJSON(w, http.StatusServiceUnavailable, map[string]string{"state": "correction_unavailable"})
				return
			}
			if status.Keyed || status.CalibrationState == "armed" || status.CalibrationState == "running" {
				writeJSON(w, http.StatusConflict, map[string]string{"state": "correction_busy"})
				return
			}
		}
		var observed pdReceiveView
		if pdArmOpcode(opcode) && process.receiveJournal != nil {
			var err error
			observed, err = observe(operationContext, process.receiveJournal.original)
			if err != nil {
				log.Printf("AetherPD: calibration receive observation rejected: %v", err)
				writeJSON(w, http.StatusServiceUnavailable, map[string]string{"state": "receive_snapshot_unavailable"})
				return
			}
		}
		if operationContext.Err() != nil {
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"state": "calibration_unavailable"})
			return
		}
		n.mu.Lock()
		authorized = n.checkTokenLocked(token)
		current := n.pdProcess == process
		n.mu.Unlock()
		if !authorized {
			writeJSON(w, http.StatusUnauthorized, apiError{errUnauthorized.Error()})
			return
		}
		if !current {
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"state": "calibration_unavailable"})
			return
		}
		var result int
		var err error
		if opcode == 'Q' {
			result, err = process.calibrateTone(operationContext, spacing)
		} else {
			result, err = process.calibrate(operationContext, opcode)
		}
		if correction && (err != nil || operationContext.Err() != nil) {
			process.Stop()
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"state": "correction_unavailable"})
			return
		}
		if pdArmOpcode(opcode) && (err != nil || operationContext.Err() != nil) {
			cancelFailedAetherPDArm(process)
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"state": "calibration_unavailable"})
			return
		}
		if err == nil && result == 0 && pdArmOpcode(opcode) && process.receiveJournal != nil {
			if journalErr := process.receiveJournal.recordPD(observed, time.Now()); journalErr != nil {
				cancelFailedAetherPDArm(process)
				log.Printf("AetherPD: calibration receive journal rejected: %v", journalErr)
				writeJSON(w, http.StatusServiceUnavailable, map[string]string{"state": "receive_snapshot_unavailable"})
				return
			}
			log.Print("AetherPD: recorded owned PD receiver in receive-recovery journal")
		}
		n.mu.Lock()
		authorized = n.checkTokenLocked(token)
		current = n.pdProcess == process
		n.mu.Unlock()
		if !authorized || !current {
			if correction {
				process.Stop()
			}
			if pdArmOpcode(opcode) && err == nil && result == 0 {
				cancelFailedAetherPDArm(process)
			}
			if !authorized {
				writeJSON(w, http.StatusUnauthorized, apiError{errUnauthorized.Error()})
			} else {
				writeJSON(w, http.StatusServiceUnavailable, map[string]string{"state": "calibration_unavailable"})
			}
			return
		}
		if err != nil || result >= 3 {
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"state": "calibration_unavailable"})
			return
		}
		if result == 1 {
			writeJSON(w, http.StatusConflict, map[string]string{"state": "calibration_busy"})
			return
		}
		if result == 2 {
			writeJSON(w, http.StatusConflict, map[string]string{"state": "calibration_blocked"})
			return
		}
		state := "calibration_arm_accepted"
		if pdMeasurementOpcode(opcode) {
			state = "measurement_arm_accepted"
		}
		if opcode == 'X' {
			state = "calibration_cancel_accepted"
		}
		if correction {
			state = "correction_bypass_accepted"
			if opcode == '1' {
				state = "correction_enable_accepted"
			}
		}
		writeJSON(w, http.StatusOK, map[string]string{"state": state})
	}
}

func cancelFailedAetherPDArm(process *aetherPDProcess) {
	// Safety cancellation must outlive a disconnected or canceled HTTP caller.
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	result, err := process.calibrate(ctx, 'X')
	if err != nil || result != 0 {
		// Releasing this owned lifetime stops the DSP and its calibration source
		// when the control channel cannot confirm cancellation.
		process.Stop()
	}
}
