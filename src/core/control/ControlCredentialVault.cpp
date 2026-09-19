#include "ControlCredentialVault.h"

#include <QThread>

#include <utility>

#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR::control {
namespace {
constexpr qsizetype kHeaderBytes = 9;
constexpr qsizetype kRecordBytes = 65; // 32 ASCII id + 1 role + 32 secret bytes
constexpr auto kMagic = "AETHTX01";
} // namespace

bool ControlCredentialVault::validAuthorityId(const QString& id)
{
    if (id.size() != 32) { return false; }
    for (const QChar c : id) {
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f'))) { return false; }
    }
    return true;
}

ControlCredentialVault::ControlCredentialVault(QString authorityId, QObject* parent)
    : QObject(parent), m_authorityId(std::move(authorityId)) {}

std::optional<QByteArray> ControlCredentialVault::encode(const QList<ControlCredentials::Record>& records)
{
    if (!ControlCredentials::validRecords(records)) { return {}; }
    // Sensitive transient serialization, not a scrubbed/locked buffer. The
    // native vault job holds its own copy; see the Stage 4 memory-limit docs.
    QByteArray bytes(kMagic);
    bytes.append(static_cast<char>(records.size()));
    for (const auto& record : records) {
        bytes.append(record.id.toLatin1());
        bytes.append(record.role == ControlCredentials::Role::GrantAdmin ? '\1' : '\0');
        bytes.append(record.secret);
    }
    return bytes;
}

std::optional<QList<ControlCredentials::Record>> ControlCredentialVault::decode(const QByteArray& bytes)
{
    if (bytes.size() < kHeaderBytes || !bytes.startsWith(kMagic)) { return {}; }
    const qsizetype count = static_cast<unsigned char>(bytes[8]);
    if (count > ControlCredentials::kMaximumRecords || bytes.size() != kHeaderBytes + count * kRecordBytes) {
        return {};
    }
    QList<ControlCredentials::Record> records;
    for (qsizetype i = 0; i < count; ++i) {
        const qsizetype offset = kHeaderBytes + i * kRecordBytes;
        const unsigned char role = static_cast<unsigned char>(bytes[offset + 32]);
        if (role > 1) { return {}; }
        records.append({QString::fromLatin1(bytes.mid(offset, 32)),
            role == 1 ? ControlCredentials::Role::GrantAdmin : ControlCredentials::Role::Client,
            bytes.mid(offset + 33, ControlCredentials::kSecretBytes)});
    }
    return ControlCredentials::validRecords(records) ? std::optional{records} : std::nullopt;
}

void ControlCredentialVault::load(Callback completed)
{
    start(false, {}, std::move(completed));
}

void ControlCredentialVault::store(const QList<ControlCredentials::Record>& records, Callback completed)
{
    const std::optional<QByteArray> bytes = encode(records);
    if (!bytes) {
        if (completed) { completed({Error::InvalidData, {}}); }
        return;
    }
    start(true, *bytes, std::move(completed));
}

void ControlCredentialVault::start(bool writing, const QByteArray& bytes, Callback completed)
{
    if (!completed) { return; }
    if (QThread::currentThread() != thread()) { completed({Error::WrongThread, {}}); return; }
    if (!validAuthorityId(m_authorityId)) { completed({Error::InvalidData, {}}); return; }
    if (m_busy) { completed({Error::Busy, {}}); return; }
#ifdef HAVE_KEYCHAIN
    QKeychain::Job* job = nullptr;
    if (writing) {
        auto* write = new QKeychain::WritePasswordJob(QStringLiteral("AetherSDR.aetherd"));
        write->setBinaryData(bytes);
        job = write;
    } else {
        job = new QKeychain::ReadPasswordJob(QStringLiteral("AetherSDR.aetherd"));
    }
    // QtKeychain owns the job through its auto-delete lifecycle, independently
    // of this callback context. Namespace the key too for old Windows builds
    // that did not incorporate service names into credential identifiers.
    job->setAutoDelete(true);
    job->setInsecureFallback(false);
    job->setKey(QStringLiteral("aetherd.v1.authority.%1").arg(m_authorityId));
    m_busy = true;
    connect(job, &QKeychain::Job::finished, this,
        [this, writing, completed = std::move(completed)](QKeychain::Job* finished) {
            Result result;
            if (finished->error() == QKeychain::EntryNotFound) { result.error = Error::NotFound; }
            else if (finished->error() == QKeychain::NoBackendAvailable
                     || finished->error() == QKeychain::NotImplemented) { result.error = Error::Unavailable; }
            else if (finished->error() != QKeychain::NoError) { result.error = Error::Storage; }
            else if (writing) { result.error = Error::None; }
            else {
                const auto records = decode(static_cast<QKeychain::ReadPasswordJob*>(finished)->binaryData());
                result.error = records ? Error::None : Error::InvalidData;
                if (records) { result.records = *records; }
            }
            m_busy = false;
            completed(std::move(result)); // may destroy this adapter; no access afterward
        });
    job->start();
#else
    Q_UNUSED(writing);
    Q_UNUSED(bytes);
    completed({Error::Unavailable, {}});
#endif
}

} // namespace AetherSDR::control
