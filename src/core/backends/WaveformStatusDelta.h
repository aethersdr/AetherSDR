#pragma once

#include <QList>
#include <QMap>
#include <QMetaType>
#include <QPair>
#include <QString>

#include <optional>

namespace AetherSDR {

// One waveform status from the radio. Kind says which of the five shapes it
// is; only that shape's fields are meaningful.
struct WaveformStatusDelta {
    enum class Kind { InstalledList, Container, Wfp, Restarted, Report };
    Kind kind{Kind::Report};
    // InstalledList: every legacy waveform installed, as (name, version).
    QList<QPair<QString, QString>> installed;
    // Container / Restarted: the waveform's name.
    QString name;
    // Container: its version, removal, and its full environment (an absent or
    // undecodable env is empty).
    QString version;
    bool removed{false};
    QMap<QString, QString> environment;
    // Wfp: the waveform processor's power, readiness and address.
    std::optional<bool> powered;
    std::optional<bool> ready;
    std::optional<QString> ipAddress;
    // Restarted: false only for result=error.
    bool ok{true};
    // Report: a waveform-defined status report, passed through as reported.
    QMap<QString, QString> report;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::WaveformStatusDelta)
