//go:build !linux

package main

import (
	"context"
	"errors"
	"os"
)

type aetherPDProcess struct {
	captures           pdCaptureStore
	calibrationIntents pdCalibrationIntents
	receiveJournal     *pdReceiveJournal
}

func (p *aetherPDProcess) Stop() {
	if p != nil {
		p.captures.close()
		p.calibrationIntents.close()
	}
}
func (*aetherPDProcess) queryStatus(context.Context) (aetherPDStatus, error) {
	return aetherPDStatus{}, errors.New("on-radio AetherPD requires Linux")
}

func (*aetherPDProcess) captureAlive() bool { return false }
func (*aetherPDProcess) testUnkeyOperation(context.Context, byte, [16]byte, uint16) pdSessionReply {
	return pdCaptureJSON(503, map[string]string{"state": "test_unavailable"})
}
func (*aetherPDProcess) readNativeCapture(context.Context) (aetherPDIQCapture, error) {
	return aetherPDIQCapture{}, errors.New("on-radio IQ capture requires Linux")
}
func startInheritedAetherPD() (*aetherPDProcess, error) {
	for _, key := range []string{"SHIM_PD_IQ_FD", "SHIM_PD_API_FD", "SHIM_PD_GUARD_FD", "SHIM_PD_SETUP_FD", "SHIM_PD_TRANSPORT_CACHE", "SHIM_PD_UNKEY_API_FD", "SHIM_PD_GUARD_READ_FD", "SHIM_PD_UNKEY_CLIENT_HANDLE", "SHIM_PD_GUI_HANDLE", "SHIM_PD_OWN_LEASE_GUARD"} {
		if os.Getenv(key) != "" {
			return nil, errors.New("on-radio AetherPD requires Linux")
		}
	}
	return nil, nil
}

func (*aetherPDProcess) calibrate(context.Context, byte) (int, error) {
	return 0, errors.New("on-radio AetherPD requires Linux")
}

func (*aetherPDProcess) restoreReceive(context.Context, func() bool) error {
	return errors.New("on-radio receive restoration requires Linux")
}

func runPDReceiveGuardianMode() bool { return false }
