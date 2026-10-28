#pragma once

#include "RadioControlTypes.h"

#include <QDeadlineTimer>
#include <QHostAddress>
#include <QSet>
#include <QPointer>
#include <atomic>
#include <memory>

namespace AetherSDR {

enum class FileTransferPurpose {
    Firmware, LegacyWaveform, DockerWaveform, ProfileMetadata, ProfileImport,
    ProfileExport, DvkUpload, DvkDownload
};

struct FileTransferControl {
    QSet<FileTransferPurpose> purposes;
    bool supports(FileTransferPurpose purpose) const { return purposes.contains(purpose); }
};

inline bool transferTokenValid(const QString& text, int maximum)
{
    if (text.isEmpty() || text.size() > maximum || text == QLatin1String(".")
        || text == QLatin1String("..")) { return false; }
    for (const QChar ch : text) {
        const ushort c = ch.unicode();
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

struct FileTransferRequest {
    FileTransferPurpose purpose{FileTransferPurpose::Firmware};
    qint64 size{0};
    QString fileName;
    int slot{0};

    bool valid() const
    {
        using Purpose = FileTransferPurpose;
        if (purpose < Purpose::Firmware || purpose > Purpose::DvkDownload) { return false; }
        const bool download = purpose == Purpose::ProfileExport || purpose == Purpose::DvkDownload;
        const bool dvk = purpose == Purpose::DvkUpload || purpose == Purpose::DvkDownload;
        const bool named = purpose == Purpose::Firmware || purpose == Purpose::LegacyWaveform
            || purpose == Purpose::DockerWaveform;
        const qint64 limit = dvk ? 5'000'000LL
            : named ? 500LL * 1024 * 1024 : 250LL * 1024 * 1024;
        if (download ? size != 0 : (size <= 0 || size > limit)) { return false; }
        if (dvk ? (slot < 1 || slot > 12) : slot != 0) { return false; }
        if (!named) { return fileName.isEmpty(); }
        if (purpose != Purpose::Firmware) { return transferTokenValid(fileName, 255); }
        // Firmware basenames historically allow spaces. Delimiters and paths
        // are never part of the radio's filename field.
        if (fileName.trimmed().isEmpty() || fileName.size() > 255
            || fileName == QLatin1String(".") || fileName == QLatin1String("..")) { return false; }
        for (const QChar ch : fileName) {
            if (!ch.isPrint() || ch == QLatin1Char('|') || ch == QLatin1Char('/')
                || ch == QLatin1Char('\\')) { return false; }
        }
        return true;
    }
};

struct FileTransferReply {
    RadioControlReply result;
    quint16 port{0};
};
using FileTransferCompletion = std::function<void(const FileTransferReply&)>;

// Only immutable deadline data and an atomic revocation flag cross the wire
// worker boundary. QObject/session checks stay on the model's thread.
class FileTransferGuard {
public:
    FileTransferGuard() = default;
    explicit FileTransferGuard(int timeoutMs)
        : m_active(std::make_shared<std::atomic<bool>>(true)),
          m_firmwarePending(std::make_shared<std::atomic<bool>>(false)), m_deadline(timeoutMs) {}
    bool current() const
    { return m_active && m_active->load(std::memory_order_acquire) && !m_deadline.hasExpired(); }
    void cancel() const
    { if (m_active) { m_active->store(false, std::memory_order_release); } }
    bool firmwarePending() const
    { return m_firmwarePending && m_firmwarePending->load(std::memory_order_acquire); }
    void setFirmwarePending(bool pending) const
    { if (m_firmwarePending) { m_firmwarePending->store(pending, std::memory_order_release); } }
private:
    std::shared_ptr<std::atomic<bool>> m_active;
    std::shared_ptr<std::atomic<bool>> m_firmwarePending;
    QDeadlineTimer m_deadline;
};

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
using FileTransferHandle = std::shared_ptr<FileTransferOperation>;

inline bool transferPeerMatches(const QHostAddress& peer, const QHostAddress& expected)
{
    return !expected.isNull() && !peer.isNull()
        && peer.isEqual(expected, QHostAddress::ConvertV4MappedToIPv4);
}

} // namespace AetherSDR
