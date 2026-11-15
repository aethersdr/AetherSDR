package main

import (
	"context"
	"testing"
)

func TestPDCalibrationIntentCancellationAndExclusiveAdmission(t *testing.T) {
	var intents pdCalibrationIntents
	ctx, finish, err := intents.beginArm(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := intents.beginArm(context.Background()); err == nil {
		t.Fatal("second arm was queued instead of refused")
	}
	intents.cancelPendingArm()
	if ctx.Err() != context.Canceled {
		t.Fatal("cancel did not invalidate pending arm")
	}
	if _, _, err := intents.beginArm(context.Background()); err == nil {
		t.Fatal("canceled old arm still owns its slot until it finishes")
	}
	finish()
	next, finishNext, err := intents.beginArm(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	defer finishNext()
	finish()
	if next.Err() != nil {
		t.Fatal("old cleanup canceled a newer explicit arm")
	}
	intents.cancelPendingArm()
	if next.Err() != context.Canceled {
		t.Fatal("new arm was not canceled")
	}
}

func TestPDCalibrationIntentRefusesCanceledParent(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	var intents pdCalibrationIntents
	if _, _, err := intents.beginArm(ctx); err != context.Canceled {
		t.Fatal("canceled caller could queue arm", err)
	}
	_, finish, err := intents.beginArm(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	finish()
}

func TestPDCalibrationSessionCloseCannotRearm(t *testing.T) {
	var intents pdCalibrationIntents
	ctx, finish, err := intents.beginArm(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	intents.close()
	if ctx.Err() != context.Canceled {
		t.Fatal("stopped session retained a pending arm")
	}
	finish()
	intents.close()
	if _, _, err := intents.beginArm(context.Background()); err == nil {
		t.Fatal("stopped session admitted a new arm")
	}
}
