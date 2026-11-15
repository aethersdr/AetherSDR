//go:build !linux

package main

import (
	"context"
	"errors"
)

func runPDInstallRequest(context.Context, [120]byte, func()) (int, error) {
	return 6, errors.New("installed requester requires Linux")
}
