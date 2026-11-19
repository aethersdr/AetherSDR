package main

import (
	"errors"
	"strings"
	"time"
)

// These are internal radio observations, not an HTTP payload or a command API.
// A future transport adapter must establish complete snapshots with reply barriers.
type pdReceiveSettings struct {
	mode                       string
	frequencyHz                int64
	antenna                    string
	filterLow, filterHigh      int
	agcMode                    string
	agcThreshold, agcOffLevel  int
	audioLevel, audioPan       int
	audioMute                  bool
	daxChannel                 int
	ritOn                      bool
	ritHz                      int
	squelch                    bool
	squelchLevel               int
	nr, nb, anf, apf           bool
	nrLevel, nbLevel, apfLevel int
}

type pdReceiveOriginal struct {
	complete     bool
	recreateSafe bool
	recreateStep int
	guiHandle    uint32
	clientID     string
	sliceID      int
	panID        uint32
	panGain      int
	settings     pdReceiveSettings
}

type pdReceiveSlice struct {
	present     bool
	owner       uint32
	panID       uint32
	frequencyHz int64
	mode        string
	daxChannel  int
}

type pdReceiveView struct {
	complete            bool
	observedAt          time.Time
	guiHandle           uint32
	clientID            string
	guiConnected        bool
	panID, panOwner     uint32
	panAntenna          string
	panGain             int
	panLowHz, panHighHz int64
	maxSlices, maxDAX   int
	remainingSlices     int
	ready, txAllowed    bool
	receive             bool
	source, reason      string
	txOwner             uint32
	slices              [16]pdReceiveSlice
}

// The journal must identify a slice observed in PD in this owned session.
// A missing receiver alone cannot distinguish API loss from an operator removal.
type pdReceiveRecoveryRequest struct {
	original          pdReceiveOriginal
	pdSliceID         int
	pdOwner, pdPan    uint32
	pdObserved        bool
	apiLeaseFailed    bool
	operatorRequested bool
}

type pdReceiveRecoveryPlan struct {
	original   pdReceiveOriginal
	admittedAt time.Time
}

type pdReceiveCreated struct {
	acknowledged bool
	sliceID      int
}

func pdReceiveClientIDValid(id string) bool {
	if len(id) != 36 {
		return false
	}
	for i, c := range []byte(id) {
		if i == 8 || i == 13 || i == 18 || i == 23 {
			if c != '-' {
				return false
			}
		} else if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f' || c >= 'A' && c <= 'F') {
			return false
		}
	}
	return true
}

func pdReceiveSettingsValid(s pdReceiveSettings) bool {
	switch s.mode {
	case "USB", "LSB", "DIGU", "DIGL", "CW", "AM", "SAM", "FM", "NFM", "DFM", "RTTY":
	default:
		return false
	}
	if s.frequencyHz <= 0 || s.frequencyHz > 1_000_000_000_000 || len(s.antenna) == 0 || len(s.antenna) > 16 {
		return false
	}
	if s.antenna[0] < 'A' || s.antenna[0] > 'Z' {
		return false
	}
	for _, c := range []byte(s.antenna) {
		if !(c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '_') {
			return false
		}
	}
	if s.filterLow < -24000 || s.filterHigh > 24000 || s.filterLow >= s.filterHigh || s.ritHz < -99999 || s.ritHz > 99999 {
		return false
	}
	switch s.agcMode {
	case "off", "slow", "med", "fast":
	default:
		return false
	}
	for _, v := range []int{s.agcThreshold, s.agcOffLevel, s.audioLevel, s.audioPan, s.squelchLevel, s.nrLevel, s.nbLevel, s.apfLevel} {
		if v < 0 || v > 100 {
			return false
		}
	}
	return s.daxChannel >= 0 && s.daxChannel <= 8
}

func validatePDReceiveIdentity(o pdReceiveOriginal, v pdReceiveView, now time.Time) error {
	return validatePDReceiveIdentityForAllocation(o, v, now, false)
}

func validatePDReceiveIdentityForAllocation(o pdReceiveOriginal, v pdReceiveView, now time.Time, allowEmptyReceive bool) error {
	if !o.complete || o.guiHandle == 0 || o.panID == 0 || o.sliceID < 0 || o.sliceID >= 16 || !pdReceiveClientIDValid(o.clientID) || !pdReceiveSettingsValid(o.settings) {
		return errors.New("original receiver snapshot incomplete or invalid")
	}
	if v.observedAt.IsZero() || now.Before(v.observedAt) || now.Sub(v.observedAt) > time.Second {
		return errors.New("fresh complete radio snapshot required")
	}
	if !v.guiConnected || v.guiHandle != o.guiHandle || !strings.EqualFold(v.clientID, o.clientID) {
		return errors.New("original GUI identity no longer owns session")
	}
	// Firmware 4.3.16 reports NO_TX_ASSIGNED for a bound GUI with no slice.
	// Only explicit missing-receiver allocation admits that receive-only state.
	emptyReceive := allowEmptyReceive && v.receive && !v.ready && !v.txAllowed && !v.slices[o.sliceID].present
	reasonAllowed := v.reason == "" || emptyReceive && v.reason == "NO_TX_ASSIGNED"
	if (!(v.ready && v.txAllowed) && !emptyReceive) || v.source != "" || !reasonAllowed || v.txOwner != 0 {
		return errors.New("fresh unkeyed READY required")
	}
	if v.panID != o.panID || v.panOwner != o.guiHandle || v.panAntenna != o.settings.antenna || v.panGain != o.panGain {
		return errors.New("original receive pan changed or lost")
	}
	if v.panLowHz <= 0 || v.panHighHz <= v.panLowHz || o.settings.frequencyHz < v.panLowHz || o.settings.frequencyHz > v.panHighHz {
		return errors.New("original receiver outside surviving pan")
	}
	return nil
}

func validatePDReceiveView(o pdReceiveOriginal, v pdReceiveView, now time.Time) error {
	if !v.complete {
		return errors.New("fresh complete radio snapshot required")
	}
	if err := validatePDReceiveIdentityForAllocation(o, v, now, true); err != nil {
		return err
	}
	if v.maxSlices < 1 || v.maxSlices > 16 || o.sliceID >= v.maxSlices || v.maxDAX < 0 || v.maxDAX > 8 || o.settings.daxChannel > v.maxDAX {
		return errors.New("receiver resources incompatible")
	}
	for id, s := range v.slices {
		if !s.present {
			continue
		}
		if id < 0 || id >= v.maxSlices || s.owner == 0 || s.panID == 0 || s.daxChannel < 0 || s.daxChannel > v.maxDAX {
			return errors.New("invalid current receiver inventory")
		}
		if id != o.sliceID && o.settings.daxChannel != 0 && s.daxChannel == o.settings.daxChannel {
			return errors.New("original DAX channel has another receiver")
		}
	}
	return nil
}

func planPDReceiveRecovery(r pdReceiveRecoveryRequest, v pdReceiveView, now time.Time) (pdReceiveRecoveryPlan, error) {
	if !r.original.recreateSafe || r.original.recreateStep < 1 || r.original.recreateStep > 1_000_000 || v.remainingSlices < 1 {
		return pdReceiveRecoveryPlan{}, errors.New("supported receive profile and free receiver required")
	}
	if !r.operatorRequested || !r.apiLeaseFailed || !r.pdObserved || r.pdSliceID != r.original.sliceID || r.pdOwner != r.original.guiHandle || r.pdPan != r.original.panID {
		return pdReceiveRecoveryPlan{}, errors.New("explicit recovery intent and owned PD failure required")
	}
	if err := validatePDReceiveView(r.original, v, now); err != nil {
		return pdReceiveRecoveryPlan{}, err
	}
	for _, slice := range v.slices {
		if slice.present && slice.owner == r.original.guiHandle {
			return pdReceiveRecoveryPlan{}, errors.New("allocation recovery requires the original GUI to have no surviving receiver")
		}
	}
	if v.slices[r.original.sliceID].present {
		return pdReceiveRecoveryPlan{}, errors.New("original slice ID is occupied; refusing overwrite")
	}
	return pdReceiveRecoveryPlan{original: r.original, admittedAt: now}, nil
}

// No settings can target an allocated ID until its create reply AND fresh
// ownership readback match the original receiver. Never remove a mismatched ID.
func confirmPDReceiveRecovery(p pdReceiveRecoveryPlan, c pdReceiveCreated, v pdReceiveView, now time.Time) (pdReceiveSettings, error) {
	if p.admittedAt.IsZero() || now.Before(p.admittedAt) || now.Sub(p.admittedAt) > 5*time.Second {
		return pdReceiveSettings{}, errors.New("recovery allocation admission expired")
	}
	if err := validatePDReceiveView(p.original, v, now); err != nil {
		return pdReceiveSettings{}, err
	}
	if !c.acknowledged || c.sliceID != p.original.sliceID {
		return pdReceiveSettings{}, errors.New("slice allocation does not match recovery request")
	}
	s := v.slices[c.sliceID]
	if !s.present || s.owner != p.original.guiHandle || s.panID != p.original.panID || s.frequencyHz != p.original.settings.frequencyHz || s.mode != p.original.settings.mode || (s.daxChannel != 0 && s.daxChannel != p.original.settings.daxChannel) {
		return pdReceiveSettings{}, errors.New("created receiver ownership or identity changed")
	}
	return p.original.settings, nil
}
