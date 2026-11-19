package main

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

func TestPDReceiveAllocationOrderingAndRefusals(t *testing.T) {
	cases := []string{"success", "unsupported profile", "no capacity", "another own receiver", "lost ACK", "wrong ACK id", "wrong owner", "token revoked after create", "canceled after create", "journal rejects", "step rejected"}
	for _, name := range cases {
		t.Run(name, func(t *testing.T) {
			request, view, _ := receiveRecoveryFixture()
			request.original.recreateSafe = true
			if name == "unsupported profile" {
				request.original.recreateSafe = false
			}
			if name == "no capacity" {
				view.remainingSlices = 0
			}
			if name == "another own receiver" {
				s := view.slices[1]
				s.owner = request.original.guiHandle
				view.slices[1] = s
			}
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			auth := true
			created := false
			consumed := false
			confirmed := false
			writes := 0
			adapter := pdReceiveAllocationIO{
				observe: func(context.Context) (pdReceiveView, error) {
					out := view
					out.observedAt = time.Now()
					if created {
						out.remainingSlices = 0
						out.slices[0] = pdReceiveSlice{present: true, owner: request.original.guiHandle, panID: request.original.panID, frequencyHz: request.original.settings.frequencyHz, mode: request.original.settings.mode}
						if name == "wrong owner" {
							out.slices[0].owner++
						}
					}
					return out, nil
				},
				consume: func() error {
					consumed = true
					if name == "journal rejects" {
						return errors.New("journal denied")
					}
					return nil
				},
				confirm: func() error {
					if !created || !consumed {
						t.Fatal("confirmation before allocation")
					}
					confirmed = true
					return nil
				},
				create: func(_ context.Context, command string) (pdReceiveCreated, error) {
					if !consumed {
						t.Fatal("allocation without consumed intent")
					}
					if command != "slice create pan=0x40000000 freq=7.150000 rxant=RX_A mode=USB" {
						t.Fatal("unsealed allocation", command)
					}
					created = true
					if name == "lost ACK" {
						return pdReceiveCreated{}, errors.New("reply lost")
					}
					if name == "token revoked after create" {
						auth = false
					}
					if name == "canceled after create" {
						cancel()
					}
					id := 0
					if name == "wrong ACK id" {
						id = 1
					}
					return pdReceiveCreated{acknowledged: true, sliceID: id}, nil
				},
				write: func(_ context.Context, command string) error {
					if !created || !confirmed || !auth || ctx.Err() != nil {
						t.Fatal("settings before acknowledged ownership or after cancellation")
					}
					if command != "slice set 0 step=100" {
						t.Fatal("unexpected command", command)
					}
					writes++
					if name == "step rejected" {
						return errors.New("step rejected")
					}
					return nil
				},
			}
			err := allocatePDReceive(ctx, request, adapter, func() bool { return auth })
			if name == "success" {
				if err != nil || writes != 1 {
					t.Fatal(err, writes)
				}
			} else if err == nil {
				t.Fatal("unsafe allocation succeeded")
			}
			if name != "success" && name != "step rejected" && writes != 0 {
				t.Fatal("unsafe settings write", name)
			}
		})
	}
}

func TestPDReceiveAllocationDoesNotRetryLostReply(t *testing.T) {
	request, _, _ := receiveRecoveryFixture()
	j := &pdReceiveJournal{original: request.original, pdObserved: true, apiFailed: true, processDone: true}
	if _, err := j.beginExistingRestore(); err != nil {
		t.Fatal(err)
	}
	if err := j.consumeAllocation(); err != nil {
		t.Fatal(err)
	}
	j.endRestore()
	if _, err := j.beginExistingRestore(); err == nil {
		t.Fatal("ambiguous allocation retried")
	}
}

func TestPDReceiveAllocationNeverEncodesRFControls(t *testing.T) {
	request, _, _ := receiveRecoveryFixture()
	command, err := pdReceiveAllocationCommand(request.original)
	if err != nil {
		t.Fatal(err)
	}
	for _, forbidden := range []string{"xmit", "tx=", "txant=", "tune", "PD", "\n"} {
		if strings.Contains(command, forbidden) {
			t.Fatal("RF or injectable allocation command", command)
		}
	}
}

func TestPDReceiveAllocationOnlyAdmitsExactEmptyReceiveState(t *testing.T) {
	request, view, _ := receiveRecoveryFixture()
	view.observedAt = time.Now()
	view.ready = false
	view.receive = true
	view.txAllowed = false
	if _, err := planPDReceiveRecovery(request, view, time.Now()); err != nil {
		t.Fatal("measured no-slice receive state refused", err)
	}
	if err := validatePDReceiveIdentity(request.original, view, time.Now()); err == nil {
		t.Fatal("ordinary restore guard broadened")
	}
	for _, name := range []string{"source", "reason", "owner", "not receive", "permission contradictory", "occupied id"} {
		t.Run(name, func(t *testing.T) {
			v := view
			switch name {
			case "source":
				v.source = "SW"
			case "reason":
				v.reason = "INTERLOCK"
			case "owner":
				v.txOwner = request.original.guiHandle
			case "not receive":
				v.receive = false
			case "permission contradictory":
				v.txAllowed = true
			case "occupied id":
				v.slices[0] = pdReceiveSlice{present: true, owner: request.original.guiHandle}
			}
			if _, err := planPDReceiveRecovery(request, v, time.Now()); err == nil {
				t.Fatal("unsafe no-slice state admitted")
			}
		})
	}
}

func TestPDReceiveAllocationNoTXAssignedOnlyForMissingReceiver(t *testing.T) {
	request, view, _ := receiveRecoveryFixture()
	view.observedAt = time.Now()
	view.ready, view.receive, view.txAllowed = false, true, false
	view.reason = "NO_TX_ASSIGNED"
	if _, err := planPDReceiveRecovery(request, view, time.Now()); err != nil {
		t.Fatal("measured bound empty-GUI receive state refused", err)
	}
	if err := validateExistingPDReceiveRestore(request.original, view, time.Now()); err == nil {
		t.Fatal("ordinary restore acquired empty-receiver exception")
	}
	for _, name := range []string{"source", "owner", "not receive", "permission", "ready", "occupied", "another own receiver", "other reason", "noncanonical reason"} {
		t.Run(name, func(t *testing.T) {
			v := view
			switch name {
			case "source":
				v.source = "SW"
			case "owner":
				v.txOwner = request.original.guiHandle
			case "not receive":
				v.receive = false
			case "permission":
				v.txAllowed = true
			case "ready":
				v.ready, v.receive = true, false
			case "occupied":
				v.slices[0] = pdReceiveSlice{present: true, owner: request.original.guiHandle}
			case "another own receiver":
				v.slices[1].owner = request.original.guiHandle
			case "other reason":
				v.reason = "INTERLOCK"
			case "noncanonical reason":
				v.reason = "no_tx_assigned"
			}
			if _, err := planPDReceiveRecovery(request, v, time.Now()); err == nil {
				t.Fatal("unsafe or occupied bound context admitted")
			}
		})
	}
}
