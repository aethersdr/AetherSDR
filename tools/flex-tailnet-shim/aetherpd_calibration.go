package main

import (
	"context"
	"errors"
	"log"
	"net/http"
	"time"
)

// Source control is separate from status and PTT. No caller-supplied waveform,
// frequency, gain, duration, PID or executable reaches the owned native child.
func aetherPDCalibrationHandler(n *Node, opcode byte) http.HandlerFunc {
	return aetherPDCalibrationHandlerWithObserver(n, opcode, observePDReceiveWorld)
}

func aetherPDCalibrationHandlerWithObserver(n *Node, opcode byte, observe func(context.Context, pdReceiveOriginal) (pdReceiveView, error)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
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
		if process == nil {
			writeJSON(w, http.StatusConflict, apiError{errAetherPDNotManaged.Error()})
			return
		}
		operationContext := r.Context()
		if opcode == 'A' || opcode == 'M' {
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
		if opcode == 'M' {
			status, err := process.queryStatus(operationContext)
			if err != nil || !status.Available || !status.Registered || !status.MeasurementControl {
				writeJSON(w, http.StatusServiceUnavailable, map[string]string{"state": "measurement_unavailable"})
				return
			}
		}
		var observed pdReceiveView
		if (opcode == 'A' || opcode == 'M') && process.receiveJournal != nil {
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
		if !n.checkToken(token) {
			writeJSON(w, http.StatusUnauthorized, apiError{errUnauthorized.Error()})
			return
		}
		result, err := process.calibrate(operationContext, opcode)
		if (opcode == 'A' || opcode == 'M') && (err != nil || operationContext.Err() != nil) {
			cancelFailedAetherPDArm(process)
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"state": "calibration_unavailable"})
			return
		}
		if err == nil && result == 0 && (opcode == 'A' || opcode == 'M') && process.receiveJournal != nil {
			if journalErr := process.receiveJournal.recordPD(observed, time.Now()); journalErr != nil {
				cancelFailedAetherPDArm(process)
				log.Printf("AetherPD: calibration receive journal rejected: %v", journalErr)
				writeJSON(w, http.StatusServiceUnavailable, map[string]string{"state": "receive_snapshot_unavailable"})
				return
			}
			log.Print("AetherPD: recorded owned PD receiver in receive-recovery journal")
		}
		if !n.checkToken(token) {
			if (opcode == 'A' || opcode == 'M') && err == nil && result == 0 {
				cancelFailedAetherPDArm(process)
			}
			writeJSON(w, http.StatusUnauthorized, apiError{errUnauthorized.Error()})
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
		if opcode == 'M' {
			state = "measurement_arm_accepted"
		}
		if opcode == 'X' {
			state = "calibration_cancel_accepted"
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
