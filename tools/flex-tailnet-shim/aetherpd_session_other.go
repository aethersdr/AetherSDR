//go:build !linux

package main

import (
	"context"
	"errors"
	"os"
)

func dialPDSession(context.Context, pdStartIdentity) (*pdSessionChannel, error) {
	return nil, errors.New("AetherPD sessions require Linux")
}
func runPDSessionMode() bool { return os.Getenv("SHIM_PD_CONTROL_FD") != "" }
