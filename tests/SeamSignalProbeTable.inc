// GENERATED FILE — regenerate with `python tools/gen_seam_probe_table.py`.
// Derived from the `signals:` section of src/core/backends/IRadioBackend.h,
// in declaration order. Add the signal to the header; never edit this table.
//
// Included by tests/SeamThreadAffinityProbe.h inside attachAllSeamSignals(),
// which defines AETHER_SEAM_PROBE. Not a standalone header: it has no include
// guard on purpose, because it is a list and not a translation unit.
//
// Kept honest by `python tools/gen_seam_probe_table.py --check` in
// .github/workflows/static-checks.yml, on every pull request, without a build.

AETHER_SEAM_PROBE(independentTxStopped);
AETHER_SEAM_PROBE(connected);
AETHER_SEAM_PROBE(disconnected);
AETHER_SEAM_PROBE(connectionError);
AETHER_SEAM_PROBE(configurationWarning);
AETHER_SEAM_PROBE(capabilitiesChanged);
AETHER_SEAM_PROBE(transmitFrequencyCheckChanged);
AETHER_SEAM_PROBE(radioDialLockChanged);
AETHER_SEAM_PROBE(linkStatsUpdated);
AETHER_SEAM_PROBE(extensionResult);
AETHER_SEAM_PROBE(extensionError);
AETHER_SEAM_PROBE(sliceChanged);
AETHER_SEAM_PROBE(sliceRemoved);
AETHER_SEAM_PROBE(sliceLifecycleFailed);
AETHER_SEAM_PROBE(meterUpdate);
AETHER_SEAM_PROBE(frontEndOverloadChanged);
AETHER_SEAM_PROBE(autoRfGainArmSettled);
AETHER_SEAM_PROBE(transmitChanged);
AETHER_SEAM_PROBE(keyingStateConfirmed);
AETHER_SEAM_PROBE(amplifierChanged);
AETHER_SEAM_PROBE(tunerChanged);
AETHER_SEAM_PROBE(radioChanged);
AETHER_SEAM_PROBE(gpsChanged);
AETHER_SEAM_PROBE(memoryChanged);
AETHER_SEAM_PROBE(memoryRefreshStarted);
AETHER_SEAM_PROBE(memoryRefreshProgress);
AETHER_SEAM_PROBE(memoryRefreshFinished);
AETHER_SEAM_PROBE(profileChanged);
AETHER_SEAM_PROBE(meterDefined);
AETHER_SEAM_PROBE(meterRemoved);
AETHER_SEAM_PROBE(panCenterBandwidthChanged);
AETHER_SEAM_PROBE(panRemoved);
AETHER_SEAM_PROBE(notchChanged);
AETHER_SEAM_PROBE(notchRemoved);
AETHER_SEAM_PROBE(sliceAudioFrameReady);
AETHER_SEAM_PROBE(panWideChanged);
AETHER_SEAM_PROBE(panRangeChanged);
AETHER_SEAM_PROBE(panBandwidthLimitsChanged);
AETHER_SEAM_PROBE(panRfGainChanged);
AETHER_SEAM_PROBE(operatingStateChanged);
AETHER_SEAM_PROBE(panRfGainInfoChanged);
AETHER_SEAM_PROBE(panPreampInfoChanged);
AETHER_SEAM_PROBE(panPreampChanged);
AETHER_SEAM_PROBE(panAttenuatorInfoChanged);
AETHER_SEAM_PROBE(panAttenuatorChanged);
AETHER_SEAM_PROBE(panRxAntennaChanged);
AETHER_SEAM_PROBE(panAntennaListChanged);
AETHER_SEAM_PROBE(panWaterfallLineDurationChanged);
AETHER_SEAM_PROBE(extensionStatus);
AETHER_SEAM_PROBE(spectrumFrameReady);
AETHER_SEAM_PROBE(waterfallRowReady);
AETHER_SEAM_PROBE(audioFrameReady);
