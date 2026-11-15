package main

import (
	"context"
	"errors"
	"fmt"
	"time"
)

type pdReceiveAllocationIO struct {
	observe func(context.Context) (pdReceiveView, error)
	create  func(context.Context, string) (pdReceiveCreated, error)
	write   func(context.Context, string) error
	confirm func() error
	consume func() error
}

func pdReceiveAllocationCommand(original pdReceiveOriginal) (string, error) {
	if !original.complete || !original.recreateSafe || original.recreateStep < 1 || original.recreateStep > 1_000_000 || !pdReceiveSettingsValid(original.settings) {
		return "", errors.New("unsupported allocation receive profile")
	}
	hz := original.settings.frequencyHz
	return fmt.Sprintf("slice create pan=0x%08X freq=%d.%06d rxant=%s mode=%s", original.panID, hz/1_000_000, hz%1_000_000, original.settings.antenna, original.settings.mode), nil
}

// This is reached only by explicit authenticated restore after a journaled API
// failure. A lost allocation reply consumes the one attempt, never retries it.
func allocatePDReceive(ctx context.Context, request pdReceiveRecoveryRequest, adapter pdReceiveAllocationIO, authorized func() bool) error {
	command, err := pdReceiveAllocationCommand(request.original)
	if err != nil {
		return err
	}
	if err = ctx.Err(); err != nil {
		return err
	}
	if !authorized() {
		return errUnauthorized
	}
	view, err := adapter.observe(ctx)
	if err != nil {
		return err
	}
	plan, err := planPDReceiveRecovery(request, view, time.Now())
	if err != nil {
		return err
	}
	if err = ctx.Err(); err != nil {
		return err
	}
	if !authorized() {
		return errUnauthorized
	}
	if err = adapter.consume(); err != nil {
		return err
	}
	created, err := adapter.create(ctx, command)
	if err != nil {
		return err
	}
	if err = ctx.Err(); err != nil {
		return err
	}
	if !authorized() {
		return errUnauthorized
	}
	view, err = adapter.observe(ctx)
	if err != nil {
		return err
	}
	if _, err = confirmPDReceiveRecovery(plan, created, view, time.Now()); err != nil {
		return err
	}
	if err = adapter.confirm(); err != nil {
		return err
	}
	if err = ctx.Err(); err != nil {
		return err
	}
	if !authorized() {
		return errUnauthorized
	}
	return adapter.write(ctx, fmt.Sprintf("slice set %d step=%d", request.original.sliceID, request.original.recreateStep))
}
