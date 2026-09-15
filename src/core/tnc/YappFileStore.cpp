#include "core/tnc/YappFileStore.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>

namespace AetherSDR {
bool YappFileStore::safeName(const QString& name)
{
    static const QRegularExpression allowed(QStringLiteral("^[A-Za-z0-9_(). -]{1,120}$"));
    static const QRegularExpression reserved(
        QStringLiteral("^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\\.|$)"),
        QRegularExpression::CaseInsensitiveOption);
    return allowed.match(name).hasMatch() && name != QStringLiteral(".")
        && name != QStringLiteral("..") && !name.startsWith(QLatin1Char('.'))
        && !name.endsWith(QLatin1Char('.')) && !name.endsWith(QLatin1Char(' '))
        && !reserved.match(name).hasMatch();
}

bool YappFileStore::open(const QString& directory, const QString& peer,
                         const QString& name, qint64 size, const QByteArray& stamp,
                         bool resume, QString& error)
{
    close();
    m_hash.reset();
    m_position = 0;
    m_size = size;
    const QFileInfo dirInfo(directory);
    if (!safeName(name) || size < 0 || size > kMaximumFileBytes || !dirInfo.isDir()) {
        error = QStringLiteral("Invalid filename, directory or file size (maximum 64 MiB)");
        return false;
    }
    const QDir dir(dirInfo.canonicalFilePath());
    m_destination = dir.filePath(name);
    if (QFileInfo::exists(m_destination) || QFileInfo(m_destination).isSymLink()) {
        error = QStringLiteral("Destination already exists; choose another receive directory");
        return false;
    }
    m_identity = {{QStringLiteral("version"), 1}, {QStringLiteral("peer"), peer},
                  {QStringLiteral("name"), name}, {QStringLiteral("size"), double(size)},
                  {QStringLiteral("stamp"), QString::fromLatin1(stamp)}};
    const QByteArray key = QCryptographicHash::hash(
        QJsonDocument(m_identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
    const QString part = dir.filePath(QStringLiteral(".aether-yapp-%1.part").arg(QString::fromLatin1(key)));
    m_metadata = part + QStringLiteral(".json");
    m_lock = std::make_unique<QLockFile>(part + QStringLiteral(".lock"));
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock()) {
        error = QStringLiteral("Partial file is in use by another transfer");
        return false;
    }
    if (QFileInfo(part).isSymLink() || QFileInfo(m_metadata).isSymLink()) {
        error = QStringLiteral("Refusing a partial-file symbolic link");
        return false;
    }
    m_file.setFileName(part);
    if (QFileInfo::exists(part)) {
        if (!resume || stamp.isEmpty()) {
            error = QStringLiteral("Partial file exists; enable resume with timestamp metadata or use another directory");
            return false;
        }
        QFile metadata(m_metadata);
        if (!metadata.open(QIODevice::ReadOnly) || metadata.size() > 4096) {
            error = QStringLiteral("Partial-file metadata is unavailable");
            return false;
        }
        QJsonObject saved = QJsonDocument::fromJson(metadata.readAll()).object();
        const QString digest = saved.take(QStringLiteral("sha256")).toString();
        const double length = saved.take(QStringLiteral("received")).toDouble(-1);
        if (saved != m_identity || length < 0 || length > size
            || length != QFileInfo(part).size() || !m_file.open(QIODevice::ReadWrite)) {
            error = QStringLiteral("Partial file does not match its saved checkpoint");
            return false;
        }
        while (!m_file.atEnd()) {
            const QByteArray chunk = m_file.read(65536);
            if (chunk.isEmpty() && m_file.error() != QFileDevice::NoError) {
                error = QStringLiteral("Could not verify partial file");
                return false;
            }
            m_hash.addData(chunk);
        }
        if (QString::fromLatin1(m_hash.result().toHex()) != digest) {
            error = QStringLiteral("Partial file changed since its checkpoint; resume refused");
            return false;
        }
        m_position = m_file.pos();
    } else if (!m_file.open(QIODevice::ReadWrite | QIODevice::NewOnly)) {
        error = QStringLiteral("Could not create partial file: %1").arg(m_file.errorString());
        return false;
    }
    const QStorageInfo storage(dir.absolutePath());
    if (storage.isValid() && storage.bytesAvailable() < size - m_position) {
        error = QStringLiteral("Insufficient free space for the incoming file");
        return false;
    }
    return checkpoint(error);
}

bool YappFileStore::checkpoint(QString& error)
{
    if (!m_file.flush()) {
        error = QStringLiteral("Could not flush partial file: %1").arg(m_file.errorString());
        return false;
    }
    QJsonObject saved = m_identity;
    saved.insert(QStringLiteral("received"), double(m_file.pos()));
    saved.insert(QStringLiteral("sha256"), QString::fromLatin1(m_hash.result().toHex()));
    const QByteArray bytes = QJsonDocument(saved).toJson(QJsonDocument::Compact);
    QSaveFile metadata(m_metadata);
    if (!metadata.open(QIODevice::WriteOnly) || metadata.write(bytes) != bytes.size()
        || !metadata.commit()) {
        error = QStringLiteral("Could not save partial-file checkpoint");
        return false;
    }
    m_position = m_file.pos();
    return true;
}

bool YappFileStore::append(const QByteArray& bytes, QString& error)
{
    if (!m_file.isOpen() || bytes.size() > m_size - m_file.pos()
        || m_file.write(bytes) != bytes.size()) {
        error = QStringLiteral("Incoming length exceeded or partial-file write failed");
        return false;
    }
    m_hash.addData(bytes);
    return checkpoint(error);
}

bool YappFileStore::finish(QString& error)
{
    if (!m_file.isOpen() || m_file.pos() != m_size || !checkpoint(error)) {
        if (error.isEmpty()) {
            error = QStringLiteral("Received file length does not match header");
        }
        return false;
    }
    m_file.close();
    if (m_file.error() != QFileDevice::NoError || !m_file.rename(m_destination)) {
        error = QStringLiteral("Could not publish received file; partial file retained");
        return false;
    }
    QFile::remove(m_metadata);
    m_lock.reset();
    return true;
}

void YappFileStore::close()
{
    if (m_file.isOpen()) {
        m_file.close();
    }
    m_lock.reset();
}
} // namespace AetherSDR
