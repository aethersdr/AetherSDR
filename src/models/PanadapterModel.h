#pragma once

#include <QObject>
#include <QMap>
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

    // Display state
    double centerMhz() const { return m_centerMhz; }
    double bandwidthMhz() const { return m_bandwidthMhz; }
    float minDbm() const { return m_minDbm; }
    float maxDbm() const { return m_maxDbm; }
    QString rxAntenna() const { return m_rxAntenna; }
    QStringList antList() const { return m_antList; }
    int rfGain() const { return m_rfGain; }
    int rfGainLow() const { return m_rfGainLow; }
    int rfGainHigh() const { return m_rfGainHigh; }
    int rfGainStep() const { return m_rfGainStep; }
    void setRfGainInfo(int low, int high, int step);
    bool wnbActive() const { return m_wnbActive; }
    int wnbLevel() const { return m_wnbLevel; }
    bool wnbUpdating() const { return m_wnbUpdating; }
    bool wideActive() const { return m_wideActive; }
    bool loopA() const { return m_loopA; }
    bool loopB() const { return m_loopB; }
    int fps() const { return m_fps; }
    int waterfallLineDuration() const { return m_waterfallLineDuration; }
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

    // Apply status from protocol
    void applyPanStatus(const QMap<QString, QString>& kvs);
    void applyWaterfallStatus(const QMap<QString, QString>& kvs);

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
    QString     m_preamp;
    int         m_daxiqChannel{0};
    bool        m_resized{false};
    bool        m_wfConfigured{false};
};

} // namespace AetherSDR
