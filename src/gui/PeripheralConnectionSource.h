#pragma once

#include <QCoreApplication>
#include <QString>

namespace AetherSDR {

// How a peripheral's data reaches the app. The amplifier and tuner applets and
// the Setup Peripherals list share this wording so one device reads the same
// everywhere.
namespace PeripheralConnectionSource {

enum class Source { Direct, Radio, Offline };

struct Text {
    QString indicator;        // "● DIRECT"
    QString word;             // "DIRECT"
    QString accessibleName;   // "TGXL DIRECT connection"
    QString description;      // "Connected directly to the TGXL."
};

// `device` is the short name used in sentences ("TGXL", "PGXL", "Antenna
// Genius"). `directOnly` is for devices with no radio relay (Antenna Genius):
// they read "directly to Antenna Genius" and describe the offline state as the
// missing direct connection.
inline Text describe(Source source, const QString& device, bool directOnly = false)
{
    auto tr = [](const char* text) {
        return QCoreApplication::translate("PeripheralConnectionSource", text);
    };
    Text out;
    switch (source) {
    case Source::Direct:
        out.word = QStringLiteral("DIRECT");
        out.accessibleName = tr("%1 DIRECT connection").arg(device);
        out.description = (directOnly ? tr("Connected directly to %1.")
                                      : tr("Connected directly to the %1.")).arg(device);
        break;
    case Source::Radio:
        out.word = QStringLiteral("RADIO");
        out.accessibleName = tr("%1 RADIO connection").arg(device);
        out.description = tr("Using the radio relay; the direct %1 connection is unavailable.")
                              .arg(device);
        break;
    case Source::Offline:
        out.word = QStringLiteral("OFFLINE");
        out.accessibleName = tr("%1 OFFLINE").arg(device);
        out.description = directOnly ? tr("No direct %1 connection.").arg(device)
                                     : tr("No %1 connection is available.").arg(device);
        break;
    }
    out.indicator = QStringLiteral("● ") + out.word;
    return out;
}

} // namespace PeripheralConnectionSource

// What Setup shows for one peripheral. Carried explicitly: nothing reads the
// label text back to decide what to show.
struct PeripheralDeviceStatus {
    enum class Attention {
        None,
        InvalidCode,       // the typed authorization code is malformed
        AuthBlocked,       // the device rejected the code; reconnects stopped
        ConnectError,      // the last connection attempt failed
        CredentialError,   // the credential store failed to clear or remove
        CredentialWarning, // keychain unavailable; deletion unconfirmed
        RemovalPending,    // an earlier Remove is still deleting a credential
    };

    PeripheralConnectionSource::Source source{PeripheralConnectionSource::Source::Offline};
    bool connecting{false};
    Attention attention{Attention::None};
    QString message;               // text for `attention`
    QString note;                  // qualifier shown after the state, e.g. session-only code
    bool pendingAuthCode{false};   // a typed code was handed over and not yet judged
    bool discardedAuthCode{false}; // a typed code was dropped before verification
    bool codeNotSaved{false};      // connected without the device asking for the typed code

    bool needsAttention() const { return attention != Attention::None; }
};

} // namespace AetherSDR
