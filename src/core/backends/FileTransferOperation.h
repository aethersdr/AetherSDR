#pragma once

#include "FileTransferRequest.h"

#include <QDeadlineTimer>
#include <QHostAddress>
#include <QObject>
#include <QPointer>

namespace AetherSDR {

class RadioModel;
class FileTransferOperation {
    friend class RadioModel;
public:
    const QHostAddress& peer() const { return m_peer; }
    bool current() const { return m_guard.current(); }
    void cancel() const { m_guard.cancel(); }
    const FileTransferGuard& guard() const { return m_guard; }
private:
    FileTransferGuard m_guard;
    QDeadlineTimer m_settleDeadline{0};
    RadioControlSession m_session{};
    QHostAddress m_peer;
    QPointer<QObject> m_owner;
    FileTransferPurpose m_purpose{FileTransferPurpose::Firmware};
    bool m_requested{false};
    bool m_metadataAccepted{false};
};

inline bool transferPeerMatches(const QHostAddress& peer, const QHostAddress& expected)
{
    return !expected.isNull() && !peer.isNull()
        && peer.isEqual(expected, QHostAddress::ConvertV4MappedToIPv4);
}

} // namespace AetherSDR
