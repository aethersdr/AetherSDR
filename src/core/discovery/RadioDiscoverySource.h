#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace AetherSDR {

// Normalized discovery data, not a connection request or a backend capability.
// Vendor payloads, credentials and connected-client metadata stay below this seam.
struct DiscoveredRadio {
    QString family;
    QString serial;
    QString name;
    QString model;
    QString nickname;
    QString version;
    QString transport; // lan, usb, or sim
    QString address;   // LAN address only; empty for USB and simulator entries
    quint16 port{0};
    bool inUse{false};
};

// Owning-thread, single-start lifecycle. Construct a new source to restart.
// Implementations must not begin discovery until start() is called.
class RadioDiscoverySource : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~RadioDiscoverySource() override = default;
    [[nodiscard]] virtual QStringList enabledSources() const = 0;
    virtual void start() = 0;
    virtual void stop() = 0;

signals:
    void radioChanged(const AetherSDR::DiscoveredRadio& radio);
    void radioLost(const QString& family, const QString& serial);
};

struct LocalDiscoveryOptions {
    bool local{false}; // Explicit operator opt-in: LAN broadcasts/listening and USB scans.
    bool simulator{false}; // Demo identity only; never starts an RX/TX backend.
};

// Implementation lives below the vendor seam. Above-seam consumers see only
// normalized observations and cannot reach a radio command through this interface.
[[nodiscard]] std::unique_ptr<RadioDiscoverySource> makeLocalRadioDiscoverySource(
    LocalDiscoveryOptions options);

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::DiscoveredRadio)
