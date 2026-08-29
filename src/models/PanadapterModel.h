#pragma once

#include <QObject>
#include <QMap>
#include <QVariantMap>
#include <QString>
#include <QStringList>

namespace AetherSDR {

// Per-panadapter state model. Replaces the single-pan fields that were
// previously in RadioModel. Each PanadapterModel represents one FFT/waterfall
// display on the radio, identified by its hex pan ID (e.g. "0x40000000").
class PanadapterModel : public QObject {
    Q_OBJECT

public:
    explicit PanadapterModel(const QString& panId, QObject* parent = nullptr);

    // Identity
    QString panId() const { return m_panId; }
    quint32 panStreamId() const;   // numeric form of panId for VITA-49 matching
    QString waterfallId() const { return m_waterfallId; }
    quint32 wfStreamId() const;    // numeric form of waterfallId
    void setWaterfallId(const QString& id);
    QString clientHandle() const { return m_clientHandle; }
    void setClientHandle(const QString& h);
    // #3977: true when this pan belongs to the given connection handle. The
    // radio reassigns client_handle when another session reclaims the pan;
    // callers gate outbound pan-set commands on this so a superseded session
    // stops adjusting the new owner's display. Fails OPEN on unknown owner —
    // safe only for gating our own commands.
    bool ownedByClient(quint32 handle) const;
    // Parsed owner (0 = radio never told us). Fail-CLOSED source for
    // eviction evidence: only pans whose confirmed owner is us may count
    // foreign writes against another client (#3977).
    quint32 ownerHandle() const { return m_ownerHandle; }

    // Display state
    double centerMhz() const { return m_centerMhz; }
    bool centerKnown() const { return m_centerKnown; }
    double bandwidthMhz() const { return m_bandwidthMhz; }
    // True when a target frequency (MHz) lies within this pan's current span
    // [center - bw/2, center + bw/2]. The source of truth for the CAT
    // (rigctld / SmartCAT) VFO-tune recenter policy — RadioModel::tuneSliceForCat
    // and TciServer::tuneSliceAndConfirm are the callers: in-span retunes keep
    // autopan=0 (no yank), out-of-span targets recenter/re-band the display. Every
    // command plane resolves "in span" here so they cannot drift apart — CAT,
    // rigctld and TCI open-coded identical copies until this one grew the
    // centerKnown term below, which is the drift this replaces. Pinned by
    // tests/cat_tune_policy_test.cpp. Until the radio has reported a real center
    // (centerKnown), m_centerMhz is a placeholder, so treat the target as out of
    // span — that recenters, which is the safe direction and establishes the
    // center. A non-positive bandwidth (span not yet known) is likewise never in span.
    bool spanContainsMhz(double mhz) const {
        if (!m_centerKnown) {
            return false;
        }
        const double halfBw = m_bandwidthMhz / 2.0;
        return halfBw > 0.0 && qAbs(mhz - m_centerMhz) <= halfBw;
    }
    // Normalized setter driven by the backend (aetherd RFC 2.3). A negative
    // value means "leave unchanged" (the radio may report one without the
    // other). Emits infoChanged when either value changes or when the center
    // is populated for the first time (even if it equals the placeholder).
    // Returns true when a value actually changed (and infoChanged was emitted).
    bool setCenterBandwidth(double centerMhz, double bandwidthMhz);
    // Force an infoChanged with the current values — for a backend re-asserting
    // a span it refused to change. See the definition.
    void republishCenterBandwidth();
    // A reclaimed model retains its numeric display state while reconnecting,
    // but that previous-session center is not authoritative until the radio
    // reports the new session's pan state.
    void resetCenterKnownForReconnect() { m_centerKnown = false; }
    // Normalized display-level-range setter driven by the backend (aetherd RFC
    // 2.3, second universal pan field). NaN for either bound means "leave
    // unchanged" (dBm is signed, so no numeric sentinel is safe). Emits
    // levelChanged when either bound actually changes; returns whether anything
    // changed so the caller can gate the panStream setDbmRange side-effect.
    bool setRange(double minDbm, double maxDbm);
    // The span limits this pan can be zoomed between (MHz), as REPORTED by the
    // backend — the X-axis counterpart to setRange's Y-axis bounds. Zero means
    // "the backend never told us", which is distinct from a real limit of zero
    // and is what keeps the Flex path on its existing model-derived clamp.
    double minBandwidthMhz() const { return m_minBandwidthMhz; }
    double maxBandwidthMhz() const { return m_maxBandwidthMhz; }
    bool bandwidthLimitsKnown() const {
        return m_minBandwidthMhz > 0.0 && m_maxBandwidthMhz > 0.0;
    }
    // Returns whether anything changed, so the caller can gate the re-clamp of
    // the widgets already showing this pan.
    bool setBandwidthLimits(double minMhz, double maxMhz);

    // Client-authoritative display rates, for a backend whose radio reports no
    // display state at all (HL2). On a Flex these arrive as radio status and the
    // radio does the rate shaping itself; on a radio that just streams spectra,
    // the operator's Display→FFT FPS and Display→Waterfall Rate sliders have
    // nowhere to go — the wire text they used to emit was a Flex command that
    // reached nothing — and the engine has to shape the stream itself. This is
    // where the target it shapes to lives. Emits the same *Reported/*Changed
    // pairs as the radio path so consumers cannot tell the two apart.
    // `wfRate` is the 1..100 waterfall RATE, low slow / high fast — not the
    // milliseconds Flex's `line_duration` wire name claims (core/WaterfallRate.h,
    // #4606).
    void setDisplayRates(int fps, int wfRate);
    // Flex-specific WNB extension applied from the backend's namespaced
    // extensionStatus("flex","panWnb",…). Applies only the keys present;
    // emits wnbChanged/wnbStateChanged when anything changes. (aetherd RFC 2.3
    // extension template — the decode lives in FlexBackend, not here.)
    void applyWnbExtension(const QVariantMap& fields);
    float minDbm() const { return m_minDbm; }
    float maxDbm() const { return m_maxDbm; }
    QString rxAntenna() const { return m_rxAntenna; }
    QStringList antList() const { return m_antList; }
    int rfGain() const { return m_rfGain; }
    int rfGainLow() const { return m_rfGainLow; }
    int rfGainHigh() const { return m_rfGainHigh; }
    int rfGainStep() const { return m_rfGainStep; }
    // What the readout appends to the number. " dB" for a real gain register,
    // "%" for a radio whose RF gain is an opaque scale — see
    // IRadioBackend::panRfGainInfoChanged.
    QString rfGainUnitSuffix() const { return m_rfGainUnitSuffix; }
    void setRfGainInfo(int low, int high, int step,
                       const QString& unitSuffix = QStringLiteral(" dB"));
    // Discrete receive front-end stages. An EMPTY label list means the radio
    // has no such stage and its control does not appear.
    QStringList preampLabels() const { return m_preampLabels; }
    int preampStep() const { return m_preampStep; }
    void setPreampLabels(const QStringList& labels);
    void setPreampStep(int step);
    QStringList attenuatorLabels() const { return m_attenuatorLabels; }
    int attenuatorStep() const { return m_attenuatorStep; }
    void setAttenuatorLabels(const QStringList& labels);
    void setAttenuatorStep(int step);
    // Normalized setters driven by the backend (aetherd RFC 2.3 — rfgain +
    // antenna promoted to universal typed signals). Each emits its existing
    // change-signal only on an actual change; the wire decode lives in
    // FlexBackend, not here.
    void setRfGain(int gain);
    void setRxAntenna(const QString& ant);
    void setAntList(const QStringList& ants);
    bool wnbActive() const { return m_wnbActive; }
    int wnbLevel() const { return m_wnbLevel; }
    bool wnbUpdating() const { return m_wnbUpdating; }
    bool wideActive() const { return m_wideActive; }
    // Set the WIDE state from a backend that computes it itself rather than
    // parsing it out of a Flex `display pan` status. Change-gated, like the
    // status path — the signal drives a repaint, and a backend that recomputes
    // this on every band decision would otherwise emit it continuously.
    void setWide(bool wide);
    bool loopA() const { return m_loopA; }
    bool loopB() const { return m_loopB; }
    int fps() const { return m_fps; }
    int average() const { return m_average; }
    bool weightedAverage() const { return m_weightedAverage; }
    // False until the radio first reports weighted_average. Lets the UI avoid
    // painting a definitive unchecked box before the real value is known, the
    // same way average()/fps() use a -1 unknown sentinel (#4261).
    bool weightedAverageKnown() const { return m_weightedAverageKnown; }
    // The 1..100 waterfall RATE, low slow / high fast. The accessor keeps
    // Flex's `line_duration` wire name because that is the field it decodes,
    // but the VALUE IS NOT MILLISECONDS — convert through core/WaterfallRate.h
    // before pacing anything on it. Reading it literally is what ran the
    // control backwards on the HL2 (#4606).
    int waterfallLineDuration() const { return m_waterfallLineDuration; }
    // Normalized waterfall-rate setter driven by the backend (universal display
    // timing). Feeds PerfTelemetry and always emits
    // waterfallLineDurationReported; the change-gated signal fires only on a real
    // change. (aetherd RFC 2.3.)
    void setWaterfallLineDuration(int rate);
    int fftYPixels() const { return m_fftYPixels; }
    bool setFftYPixels(int yPixels) {
        if (m_fftYPixels == yPixels) {
            return false;
        }
        m_fftYPixels = yPixels;
        return true;
    }
    QString preamp() const { return m_preamp; }
    void setPreamp(const QString& pre) {
        // Preamp is internal state only — no UI listeners. Do not emit
        // rfGainChanged here; doing so caused a ~33ms echo loop because the
        // gain display listener would re-assert the current rfgain back to
        // the radio on every status cycle (#1498).
        if (m_preamp != pre) { m_preamp = pre; }
    }
    int daxiqChannel() const { return m_daxiqChannel; }

    // Band / segment zoom — radio-owned per-pan flags (FlexLib Panadapter.cs
    // IsBandZoomOn/IsSegmentZoomOn). The radio clears them itself on a manual
    // pan/zoom (and clears the sibling when the other engages) and broadcasts
    // both transitions in pan status; this model state is the single truth the
    // zoom toggles read (#4057).
    bool bandZoomOn() const { return m_bandZoomOn; }
    bool segmentZoomOn() const { return m_segmentZoomOn; }

    // Configuration flags
    bool isResized() const { return m_resized; }
    void setResized(bool r) { m_resized = r; }
    bool isWaterfallConfigured() const { return m_wfConfigured; }
    void setWaterfallConfigured(bool c) { m_wfConfigured = c; }

    // Flex-specific display-pan state applied from the backend's namespaced
    // extensionStatus("flex","panState",…): wide, loop A/B, fps, preamp, DAX-IQ
    // channel, MultiFlex client_handle ownership, waterfall stream-id. Applies
    // only the keys present. (aetherd RFC 2.3 — the decode lives in FlexBackend;
    // this is the last of PanadapterModel's Flex status decode to move, so the
    // old applyPanStatus/applyWaterfallStatus wire-decoders are now gone.)
    void applyStateExtension(const QVariantMap& fields);

signals:
    void infoChanged(double centerMhz, double bandwidthMhz);
    void levelChanged(float minDbm, float maxDbm);
    void bandwidthLimitsChanged(double minMhz, double maxMhz);
    void rxAntennaChanged(const QString& ant);
    void antListChanged(const QStringList& ants);
    void rfGainChanged(int gain);
    void rfGainInfoChanged(int low, int high, int step,
                           const QString& unitSuffix = QStringLiteral(" dB"));
    void preampLabelsChanged(const QStringList& labels);
    void preampStepChanged(int step);
    void attenuatorLabelsChanged(const QStringList& labels);
    void attenuatorStepChanged(int step);
    void wnbChanged(bool active, int level);
    void wnbStateChanged(bool active, int level, bool updating);
    void wideChanged(bool active);
    void loopChanged(bool loopA, bool loopB);
    void fpsChanged(int fps);
    void fpsReported(int fps);
    // Averaging is radio-authoritative (firmware runs it, echoes the level in
    // pan status). Reported fires every status cycle; Changed only on an actual
    // change — mirrors the fps pair so the display follows the radio's value
    // after a global-profile / band switch adopts the profile's stored value
    // (#4001; radio-owned per #4261 — no client re-assert / persistence).
    void averageChanged(int average);
    void averageReported(int average);
    void weightedAverageChanged(bool weighted);
    void weightedAverageReported(bool weighted);
    void waterfallLineDurationChanged(int ms);
    void waterfallLineDurationReported(int ms);
    void waterfallIdChanged(const QString& wfId);
    void daxiqChannelChanged(int channel);
    void bandZoomChanged(bool on);
    void segmentZoomChanged(bool on);

private:
    QString     m_panId;
    QString     m_waterfallId;
    QString     m_clientHandle;
    quint32     m_ownerHandle{0};   // parsed m_clientHandle; 0 = unknown (#3977)
    double      m_centerMhz{14.1};
    bool        m_centerKnown{false}; // true after a normalized center update
    double      m_bandwidthMhz{0.2};
    float       m_minDbm{-130.0f};
    float       m_maxDbm{-40.0f};
    // 0 = the backend has not reported span limits for this pan (see
    // bandwidthLimitsKnown); the GUI then keeps its model-derived clamp.
    double      m_minBandwidthMhz{0.0};
    double      m_maxBandwidthMhz{0.0};
    QString     m_rxAntenna;
    QStringList m_antList;
    int         m_rfGain{0};
    int         m_rfGainLow{-8};
    int         m_rfGainHigh{32};
    int         m_rfGainStep{8};
    QString     m_rfGainUnitSuffix{QStringLiteral(" dB")};
    QStringList m_preampLabels;
    int         m_preampStep{0};
    QStringList m_attenuatorLabels;
    int         m_attenuatorStep{0};
    bool        m_wnbActive{false};
    bool        m_wnbUpdating{false};
    bool        m_wideActive{false};
    bool        m_loopA{false};
    bool        m_loopB{false};
    int         m_wnbLevel{50};
    int         m_fps{-1};
    int         m_average{-1};        // -1 = unknown; 0 = off, 1-N = level (#4001)
    bool        m_weightedAverage{false};
    bool        m_weightedAverageKnown{false};  // #4261 unknown sentinel
    int         m_waterfallLineDuration{-1};
    int         m_fftYPixels{-1};
    QString     m_preamp;
    int         m_daxiqChannel{0};
    bool        m_bandZoomOn{false};
    bool        m_segmentZoomOn{false};
    bool        m_resized{false};
    bool        m_wfConfigured{false};
};

} // namespace AetherSDR
