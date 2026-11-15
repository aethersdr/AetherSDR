package main

import (
	"sync"
	"testing"
	"time"
)

func receiveJournalFixture() (*pdReceiveJournal, pdReceiveView, time.Time) {
	request, view, now := receiveRecoveryFixture()
	view.slices[0] = pdReceiveSlice{present: true, owner: request.original.guiHandle, panID: request.original.panID, frequencyHz: request.original.settings.frequencyHz, mode: "PD"}
	return &pdReceiveJournal{original: request.original}, view, now
}

func TestPDReceiveJournalRecordsOwnedPDAndDistinctExit(t *testing.T) {
	j, view, now := receiveJournalFixture()
	if err := j.recordPD(view, now); err != nil {
		t.Fatal(err)
	}
	before := j.recoveryRequest(true)
	if !before.pdObserved || before.apiLeaseFailed {
		t.Fatal("live process cannot authorize recovery")
	}
	j.recordExit(true)
	withoutIntent := j.recoveryRequest(false)
	view.slices[0] = pdReceiveSlice{}
	if _, err := planPDReceiveRecovery(withoutIntent, view, now); err == nil {
		t.Fatal("journal manufactured operator restore intent")
	}
	if _, err := planPDReceiveRecovery(j.recoveryRequest(true), view, now); err != nil {
		t.Fatal(err)
	}
	if err := j.recordPD(view, now); err == nil {
		t.Fatal("closed session accepted new PD observation")
	}
}

func TestPDReceiveJournalRejectsStaleForeignOrReusedReceiver(t *testing.T) {
	for name, alter := range map[string]func(*pdReceiveView){
		"missing":          func(v *pdReceiveView) { v.slices[0] = pdReceiveSlice{} },
		"foreign":          func(v *pdReceiveView) { v.slices[0].owner++ },
		"wrong pan":        func(v *pdReceiveView) { v.slices[0].panID++ },
		"not PD":           func(v *pdReceiveView) { v.slices[0].mode = "USB" },
		"retuned":          func(v *pdReceiveView) { v.slices[0].frequencyHz++ },
		"GUI reused":       func(v *pdReceiveView) { v.clientID = "99999999-2222-4333-8444-555555555555" },
		"GUI disconnected": func(v *pdReceiveView) { v.guiConnected = false },
		"pan gain changed": func(v *pdReceiveView) { v.panGain++ },
		"stale":            func(v *pdReceiveView) { v.observedAt = v.observedAt.Add(-time.Second - time.Nanosecond) },
		"future":           func(v *pdReceiveView) { v.observedAt = v.observedAt.Add(time.Nanosecond) },
	} {
		t.Run(name, func(t *testing.T) {
			j, view, now := receiveJournalFixture()
			alter(&view)
			if err := j.recordPD(view, now); err == nil {
				t.Fatal("invalid PD observation accepted")
			}
			j.recordExit(true)
			if j.recoveryRequest(true).pdObserved {
				t.Fatal("failed observation was journaled")
			}
		})
	}
}

func TestPDReceiveJournalFirstExitWinsAndCannotBecomeIntent(t *testing.T) {
	j, view, now := receiveJournalFixture()
	if err := j.recordPD(view, now); err != nil {
		t.Fatal(err)
	}
	j.recordExit(false)
	var group sync.WaitGroup
	for i := 0; i < 20; i++ {
		group.Add(1)
		go func() {
			defer group.Done()
			j.recordExit(true)
			if r := j.recoveryRequest(false); r.apiLeaseFailed || r.operatorRequested {
				t.Error("normal exit became API failure or restore intent")
			}
		}()
	}
	group.Wait()
}
