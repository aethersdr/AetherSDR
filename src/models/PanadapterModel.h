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
    double bandwidthMhz() const { return m_bandwidthMhz; }
    // Normalized setter driven by the backend (aetherd RFC 2.3). A negative
    // value means "leave unchanged" (the radio may report one without the
    // other). Emits infoChanged when either actually changes.
    void setCenterBandwidth(double centerMhz, double bandwidthMhz);
    // Normalized display-level-range setter driven by the backend (aetherd RFC
    // 2.3, second universal pan field). NaN for either bound means "leave
    // unchanged" (dBm is signed, so no numeric sentinel is safe). Emits
    // levelChanged when either bound actually changes; returns whether anything
    // changed so the caller can gate the panStream setDbmRange side-effect.
    bool setRange(double minDbm, double maxDbm);
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
    void setRfGainInfo(int low, int high, int step);
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
    bool loopA() const { return m_loopA; }
    bool loopB() const { return m_loopB; }
    int fps() const { return m_fps; }
    int waterfallLineDuration() const { return m_waterfallLineDuration; }
    // Normalized waterfall-line-duration setter driven by the backend (universal
    // display timing). Feeds PerfTelemetry and always emits
    // waterfallLineDurationReported; the change-gated signal fires only on a real
    // change. (aetherd RFC 2.3.)
    void setWaterfallLineDuration(int ms);
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
    void rxAntennaChanged(const QString& ant);
    void antListChanged(const QStringList& ants);
    void rfGainChanged(int gain);
    void rfGainInfoChanged(int low, int high, int step);
    void wnbChanged(bool active, int level);
    void wnbStateChanged(bool active, int level, bool updating);
    void wideChanged(bool active);
    void loopChanged(bool loopA, bool loopB);
    void fpsChanged(int fps);
    void fpsReported(int fps);
    void waterfallLineDurationChanged(int ms);
    void waterfallLineDurationReported(int ms);
    void waterfallIdChanged(const QString& wfId);
    void daxiqChannelChanged(int channel);

private:
    QString     m_panId;
    QString     m_waterfallId;
    QString     m_clientHandle;
    quint32     m_ownerHandle{0};   // parsed m_clientHandle; 0 = unknown (#3977)
    double      m_centerMhz{14.1};
    double      m_bandwidthMhz{0.2};
    float       m_minDbm{-130.0f};
    float       m_maxDbm{-40.0f};
    QString     m_rxAntenna;
    QStringList m_antList;
    int         m_rfGain{0};
    int         m_rfGainLow{-8};
    int         m_rfGainHigh{32};
    int         m_rfGainStep{8};
    bool        m_wnbActive{false};
    bool        m_wnbUpdating{false};
    bool        m_wideActive{false};
    bool        m_loopA{false};
    bool        m_loopB{false};
    int         m_wnbLevel{50};
    int         m_fps{-1};
    int         m_waterfallLineDuration{-1};
    int         m_fftYPixels{-1};
    QString     m_preamp;
    int         m_daxiqChannel{0};
    bool        m_resized{false};
    bool        m_wfConfigured{false};
};

} // namespace AetherSDR
