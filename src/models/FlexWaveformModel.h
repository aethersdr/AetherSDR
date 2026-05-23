#pragma once

#include <QObject>
#include <QList>
#include <QMap>
#include <QString>

namespace MasterSDR {

// A single installed waveform (legacy or Docker container).
// Mirrors FlexLib's Waveform record (Waveform.cs).
struct FlexWaveformEntry {
    QString name;
    QString version;
    bool    isContainer{false};

    QString displayName() const {
        return version.isEmpty() ? name : name + QLatin1Char(' ') + version;
    }
};

// Handles the three waveform status sub-shapes introduced in firmware v4.2.18:
//
//   "waveform"            — installed_list key (legacy waveform list)
//   "waveform container"  — Docker container add/remove
//   "waveform wfp_status" — Waveform Processor power/ready/ipaddr
//
// Named FlexWaveformModel (not WaveformModel) to avoid confusion with the
// audio waveform visualization classes (WaveformWidget, StripWaveform).
class FlexWaveformModel : public QObject {
    Q_OBJECT

public:
    explicit FlexWaveformModel(QObject* parent = nullptr);

    // ── Accessors ─────────────────────────────────────────────────────────────
    const QList<FlexWaveformEntry>& waveforms()    const { return m_waveforms; }
    bool    wfpPowered()   const { return m_wfpPowered; }
    bool    wfpReady()     const { return m_wfpReady; }
    QString wfpIpAddress() const { return m_wfpIpAddress; }

    // ── Status parsing (called from RadioModel::onStatusReceived) ─────────────
    // object == "waveform"
    void handleInstalledList(const QMap<QString, QString>& kvs);
    // object == "waveform container"
    void handleContainerStatus(const QMap<QString, QString>& kvs);
    // object == "waveform wfp_status"
    void handleWfpStatus(const QMap<QString, QString>& kvs);

    void clear();

    // ── Command emitters ──────────────────────────────────────────────────────
    // FlexLib Radio.cs:8484-8499
    void requestUninstall(const QString& name);        // waveform uninstall <name>
    void requestRemoveContainer(const QString& name);  // waveform remove_container <name>
    void requestRestart(const QString& name);          // waveform restart <name>

signals:
    void waveformsChanged();
    void wfpStatusChanged();
    void commandReady(const QString& cmd);

private:
    QList<FlexWaveformEntry> m_waveforms;
    bool    m_wfpPowered{false};
    bool    m_wfpReady{false};
    QString m_wfpIpAddress;
};

} // namespace MasterSDR
