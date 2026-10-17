#pragma once

#include <QHostAddress>
#include <QString>
#include <QtGlobal>
#include <initializer_list>
#include <memory>

namespace AetherSDR {

struct VitaDscpResult {
    bool applied{false};
    QString detail;
};

// Owns only QoS resources, never the UDP socket. All calls stay on its
// network thread; reset before closing/rebinding the socket.
class VitaDscpMarker {
public:
    VitaDscpMarker();
    ~VitaDscpMarker();
    VitaDscpMarker(const VitaDscpMarker&) = delete;
    VitaDscpMarker& operator=(const VitaDscpMarker&) = delete;

    VitaDscpResult apply(qintptr descriptor, const QHostAddress& destination,
                         std::initializer_list<quint16> ports);
    void reset();

private:
    struct NativeState;
    std::unique_ptr<NativeState> m_native;
};

} // namespace AetherSDR
