#pragma once

#include <QCryptographicHash>
#include <QFile>
#include <QJsonObject>
#include <QLockFile>
#include <memory>

namespace AetherSDR {

// Local receive policy, separate from the wire engine so mailbox admission can
// reuse YAPP-C without giving a remote filename authority over a local path.
class YappFileStore {
public:
    static constexpr qint64 kMaximumFileBytes = 64 * 1024 * 1024;
    static bool safeName(const QString& name);
    bool open(const QString& directory, const QString& peer, const QString& name,
              qint64 size, const QByteArray& stamp, bool resume, QString& error);
    bool append(const QByteArray& bytes, QString& error);
    bool finish(QString& error);
    void close();
    qint64 position() const { return m_file.isOpen() ? m_file.pos() : m_position; }
    QString destination() const { return m_destination; }
private:
    bool checkpoint(QString& error);
    QFile m_file;
    std::unique_ptr<QLockFile> m_lock;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QJsonObject m_identity;
    QString m_metadata;
    QString m_destination;
    qint64 m_position{0};
    qint64 m_size{0};
};
} // namespace AetherSDR
