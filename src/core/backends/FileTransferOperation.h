#pragma once

#include "FileTransferRequest.h"

#include <array>
#include <optional>

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
    bool reservedFor(FileTransferPurpose purpose, const RadioControlSession& session) const
    { return m_purpose == purpose && m_session == session; }
private:
    FileTransferGuard m_guard;
    QDeadlineTimer m_settleDeadline{0};
    RadioControlSession m_session{};
    QHostAddress m_peer;
    QPointer<QObject> m_owner;
    FileTransferPurpose m_purpose{FileTransferPurpose::Firmware};
    bool m_requested{false};
    bool m_metadataAccepted{false};
    std::optional<std::array<FileTransferRequest, 2>> m_waveformSequence;
    unsigned m_sequenceIndex{0};
    bool m_setupAccepted{false};
};

inline bool transferPeerMatches(const QHostAddress& peer, const QHostAddress& expected)
{
    return !expected.isNull() && !peer.isNull()
        && peer.isEqual(expected, QHostAddress::ConvertV4MappedToIPv4);
}

} // namespace AetherSDR
