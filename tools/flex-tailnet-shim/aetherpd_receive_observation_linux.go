//go:build linux

package main

import (
	"context"
	"log"
	"time"
)

// Record evidence, not restore intent. An unarmed API loss can remove its PD
// receiver too; later explicit recovery still needs the original GUI/pan,
// dedicated API-failure exit and a fresh unkeyed resource inventory.
func (p *aetherPDProcess) observePDReceiveOwnership(ctx context.Context, status aetherPDStatus) {
	if p == nil || p.receiveJournal == nil || ctx.Err() != nil || !status.Available || !status.Registered || status.Keyed || status.nativeActiveSlice == nil {
		return
	}
	original, admitted := p.receiveJournal.beginPDObservation(*status.nativeActiveSlice, time.Now())
	if !admitted {
		return
	}
	defer p.receiveJournal.endPDObservation()
	observe := p.receiveObserve
	if observe == nil {
		observe = observePDReceiveWorld
	}
	// Status has a two-second desktop deadline. Keep this optional witness
	// bounded; failed observations can retry at most once per second.
	ctx, cancel := context.WithTimeout(ctx, 750*time.Millisecond)
	defer cancel()
	view, err := observe(ctx, original)
	if err == nil && ctx.Err() == nil {
		if err := p.receiveJournal.recordPD(view, time.Now()); err == nil {
			log.Print("AetherPD: recorded owned PD receiver for explicit receive recovery")
		}
	}
}
