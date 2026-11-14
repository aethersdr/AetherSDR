#pragma once

#include "core/backends/WaveformRequest.h"

#include <QObject>
#include <QList>
#include <QMap>
#include <QString>
#include "core/backends/WaveformStatusDelta.h"

namespace AetherSDR {

// A single installed waveform (legacy or Docker container).
// Mirrors FlexLib's Waveform record (Waveform.cs).
struct FlexWaveformEntry {
    QString name;
    QString version;
    bool    isContainer{false};
    // The container's environment as the radio last reported it (SmartSDR
    // 4.3.16 `env=`), replaced wholesale on every container status. Values may be
    // secrets: never log them.
    QMap<QString, QString> environment;

    QString displayName() const {
        return version.isEmpty() ? name : name + QLatin1Char(' ') + version;
    }
};

// Handles the waveform status sub-shapes introduced in firmware v4.2.18, plus
// the 4.3.16 additions:
//
//   "waveform"            — installed_list key (legacy waveform list)
//   "waveform container"  — Docker container add/remove (+ env= in 4.3.16)
//   "waveform wfp_status" — Waveform Processor power/ready/ipaddr
//   "waveform restarted"  — outcome of a restart or recreate (4.3.16)
//
// Named FlexWaveformModel (not WaveformModel) to avoid confusion with the
// audio waveform visualization class (WaveformWidget).
class FlexWaveformModel : public QObject {
    Q_OBJECT

public:
    explicit FlexWaveformModel(QObject* parent = nullptr);

    // ── Accessors ─────────────────────────────────────────────────────────────
    const QList<FlexWaveformEntry>& waveforms()    const { return m_waveforms; }
    bool    wfpStatusSeen() const { return m_wfpStatusSeen; }
    bool    wfpPowered()   const { return m_wfpPowered; }
    bool    wfpReady()     const { return m_wfpReady; }
    QString wfpIpAddress() const { return m_wfpIpAddress; }
    const QList<QMap<QString, QString>>& statusReports() const { return m_statusReports; }

    // ── Status (decoded by the Flex status ingress) ─────────────────────────
    void applyStatus(const WaveformStatusDelta& status);
    // A waveform-defined status report, kept as reported ("waveform status"
    // and a slice's waveform_status).
    void handleGenericStatus(const QMap<QString, QString>& kvs);

    void clear();

    // ── Command emitters ──────────────────────────────────────────────────────
    // FlexLib Radio.cs:8484-8499
    void requestUninstall(const QString& name);        // waveform uninstall <name>
    void requestRemoveContainer(const QString& name);  // waveform remove_container <name>
    void requestRestart(const QString& name);          // waveform restart <name>

    // The radio's reply to a command emitted above, routed back by RadioModel
    // so the UI can say what happened (a restart changes nothing visible).
    void handleCommandReply(const WaveformRequest& request, const RadioControlReply& reply);

signals:
    void waveformsChanged();
    // Only the environment of an existing container changed; the list, and any
    // selection in it, is unchanged.
    void environmentChanged(const QString& name);
    // The radio's `waveform restarted` status: result=error is the only failure.
    void restarted(const QString& name, bool ok);
    void wfpStatusChanged();
    void statusReportsChanged();
    void genericStatusReceived(const QMap<QString, QString>& report);
    void controlRequested(const AetherSDR::WaveformRequest& request);
    // action: "restart", "remove_container" or "uninstall".
    void commandFinished(const QString& action, const QString& name, bool ok,
                         const QString& message);

private:
    void applyInstalledList(const WaveformStatusDelta& d);
    void applyContainer(const WaveformStatusDelta& d);
    void applyWfpStatus(const WaveformStatusDelta& d);
    QList<FlexWaveformEntry> m_waveforms;
    bool    m_wfpStatusSeen{false};
    bool    m_wfpPowered{false};
    bool    m_wfpReady{false};
    QString m_wfpIpAddress;
    QList<QMap<QString, QString>> m_statusReports;
};

} // namespace AetherSDR
