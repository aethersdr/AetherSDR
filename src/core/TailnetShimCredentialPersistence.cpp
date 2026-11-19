#include "TailnetShimCredentialPersistence.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QLockFile>
#include <QObject>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <optional>
#include <utility>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef HAVE_KEYCHAIN
#include <qt6keychain/keychain.h>
#endif

namespace AetherSDR::tailnetshim {
namespace {
using Error = CredentialPersistence::Error;
using Raw = CredentialPersistence::Raw;
using RawDone = CredentialPersistence::RawDone;
using Value = CredentialPersistence::Value;
constexpr int kTokenLimit = 4096;
constexpr int kRecordLimit = 512;
constexpr int kQueueLimit = 256;
constexpr int kEntryQueueLimit = 16;

QByteArray fingerprint(const QString& value)
{
    return QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256);
}

struct Record {
    QByteArray version;
    QByteArray fingerprint;
    bool committed{false};
};

std::optional<Record> decodeRecord(const Raw& value)
{
    if (value.error == Error::Missing) { return Record{}; }
    if (value.error != Error::None || value.text.toUtf8().size() > kRecordLimit) { return {}; }
    const QJsonDocument document = QJsonDocument::fromJson(value.text.toUtf8());
    if (!document.isObject()) { return {}; }
    const QJsonObject object = document.object();
    if (object.size() != 4 || !object.value(u"schema").isDouble() || object.value(u"schema").toDouble() != 1
        || !object.value(u"version").isString() || !object.value(u"sha256").isString()
        || !object.value(u"committed").isBool()) { return {}; }
    const QByteArray version = object.value(u"version").toString().toLatin1();
    const QByteArray hash = object.value(u"sha256").toString().toLatin1();
    if (version.size() != 32 || hash.size() != 64 || QByteArray::fromHex(version).toHex() != version
        || QByteArray::fromHex(hash).toHex() != hash || QByteArray::fromHex(version) == QByteArray(16, '\0')) { return {}; }
    return Record{QByteArray::fromHex(version), QByteArray::fromHex(hash), object.value(u"committed").toBool()};
}

QString encodeRecord(const Record& record)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"schema", 1},
        {"version", QString::fromLatin1(record.version.toHex())},
        {"sha256", QString::fromLatin1(record.fingerprint.toHex())}, {"committed", record.committed}}).toJson(QJsonDocument::Compact));
}

QByteArray selectionFor(const QString& primary, const Raw& record)
{
    const QByteArray bytes = primary.toUtf8();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("AetherSDR/CredentialSelection/1:"));
    hash.addData(QByteArray::number(bytes.size()));
    hash.addData(QByteArrayLiteral(":"));
    hash.addData(bytes);
    hash.addData(record.error == Error::Missing ? QByteArrayLiteral(":missing") : QByteArrayLiteral(":present:"));
    hash.addData(record.text.toUtf8());
    return hash.result();
}

CredentialPersistence::Lease acquireSystemLease(const QString& key)
{
    QString directory;
#ifdef Q_OS_UNIX
    // All profiles and temporary-directory overrides share one private UID
    // namespace. Existing symlinks, foreign ownership or broad modes refuse.
    directory = QStringLiteral("/tmp/aethersdr-credentials-%1").arg(getuid());
    const QByteArray native = directory.toLocal8Bit();
    ::mkdir(native.constData(), 0700);
    struct stat info {};
    if (::lstat(native.constData(), &info) != 0 || !S_ISDIR(info.st_mode)
        || info.st_uid != getuid() || (info.st_mode & 0077) != 0) { return {}; }
#else
    directory = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (directory.isEmpty()) { return {}; }
    directory = QDir(directory).filePath(QStringLiteral("aethersdr-credential-locks"));
    if (!QDir().mkpath(directory)) { return {}; }
#endif
    const QString name = QString::fromLatin1(fingerprint(QStringLiteral("AetherSDR/") + key).toHex());
    auto lock = std::make_shared<QLockFile>(QDir(directory).filePath(name + QStringLiteral(".lock")));
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) { return {}; }
    return lock;
}

CredentialPersistence::Backend systemBackend()
{
    CredentialPersistence::Backend backend;
#ifdef HAVE_KEYCHAIN
    const auto result = [](QKeychain::Job* job) {
        Raw raw;
        raw.error = job->error() == QKeychain::NoError ? Error::None
            : job->error() == QKeychain::EntryNotFound ? Error::Missing : Error::Unavailable;
        if (raw.error == Error::Unavailable) { raw.message = QStringLiteral("system keychain operation failed"); }
        return raw;
    };
    backend.read = [result](const QString& key, QObject* owner, RawDone done) {
        auto* job = new QKeychain::ReadPasswordJob(QStringLiteral("AetherSDR"), owner);
        job->setKey(key);
        job->setAutoDelete(true);
        job->setInsecureFallback(false);
        QObject::connect(job, &QKeychain::Job::finished, owner, [result, done = std::move(done)](QKeychain::Job* finished) {
            Raw raw = result(finished);
            if (raw.error == Error::None) { raw.text = static_cast<QKeychain::ReadPasswordJob*>(finished)->textData(); }
            done(std::move(raw));
        });
        job->start();
    };
    backend.write = [result](const QString& key, const QString& text, QObject* owner, RawDone done) {
        auto* job = new QKeychain::WritePasswordJob(QStringLiteral("AetherSDR"), owner);
        job->setKey(key);
        job->setTextData(text);
        job->setAutoDelete(true);
        job->setInsecureFallback(false);
        QObject::connect(job, &QKeychain::Job::finished, owner, [result, done = std::move(done)](QKeychain::Job* finished) { done(result(finished)); });
        job->start();
    };
    backend.remove = [result](const QString& key, QObject* owner, RawDone done) {
        auto* job = new QKeychain::DeletePasswordJob(QStringLiteral("AetherSDR"), owner);
        job->setKey(key);
        job->setAutoDelete(true);
        job->setInsecureFallback(false);
        QObject::connect(job, &QKeychain::Job::finished, owner, [result, done = std::move(done)](QKeychain::Job* finished) { done(result(finished)); });
        job->start();
    };
#endif
    return backend;
}
} // namespace

class CredentialPersistence::State : public std::enable_shared_from_this<CredentialPersistence::State> {
public:
    enum class Kind { Read, Write, Retire, Replace };
    class Operation;
    State(Backend backend, Acquire acquire, int timeout)
        : m_backend(std::move(backend)), m_acquire(std::move(acquire)), m_timeout(qBound(1, timeout, 10000)) {}
    void enqueue(Kind kind, const QString& key, const QString& token, const QByteArray& version, Done done,
                 const QString& previous = {});
    void released(Operation* operation);
    Backend m_backend;
    Acquire m_acquire;
    int m_timeout;
    int m_outstanding{0};
    QHash<QString, QList<Operation*>> m_queues;
};

class CredentialPersistence::State::Operation : public QObject {
public:
    Operation(std::shared_ptr<State> state, Kind kind, QString key, QString token, QByteArray version, Done done,
              QString previous)
        : QObject(QCoreApplication::instance()), m_state(std::move(state)), m_kind(kind), m_key(std::move(key)),
          m_token(std::move(token)), m_expected(std::move(version)), m_previous(std::move(previous)), m_done(std::move(done))
    {
        m_elapsed.start();
        m_wait.setInterval(10);
        connect(&m_wait, &QTimer::timeout, this, [this] { admit(); });
        m_wait.start();
    }
    const QString& key() const { return m_key; }
    void activate() { m_head = true; admit(); }

private:
    void admit()
    {
        if (m_finished || m_started) { return; }
        if (m_elapsed.elapsed() >= m_state->m_timeout) {
            finish({Error::Unavailable, {}, {}, QStringLiteral("credential persistence is busy or unavailable")}); return;
        }
        if (!m_head) { return; }
        m_lease = m_state->m_acquire(m_key);
        if (!m_lease) { return; }
        m_started = true;
        m_wait.stop();
        if (m_kind == Kind::Write) { readRecord(); }
        else { readPrimary(); }
    }
    void readPrimary()
    {
        m_state->m_backend.read(m_key, this, [this](Raw raw) {
            if (raw.error == Error::Missing) {
                finish({m_kind == Kind::Retire || m_kind == Kind::Replace ? Error::Rejected : Error::Missing, {}, {}, {}}); return;
            }
            if (raw.error != Error::None || raw.text.toUtf8().size() > kTokenLimit) { unavailable(); return; }
            m_primary = std::move(raw.text);
            readRecord();
        });
    }
    void readRecord()
    {
        m_state->m_backend.read(CredentialPersistence::versionKey(m_key), this, [this](Raw raw) {
            const std::optional<Record> record = decodeRecord(raw);
            if (!record) { unavailable(); return; }
            if (m_kind == Kind::Read) {
                const QByteArray version = record->committed && record->fingerprint == fingerprint(m_primary)
                    ? record->version : QByteArray{};
                finish({Error::None, m_primary, version, {}, selectionFor(m_primary, raw)}); return;
            }
            if (m_kind == Kind::Retire && (m_primary != m_token || m_expected.size() != 16
                || !record->committed || record->version != m_expected || record->fingerprint != fingerprint(m_primary))) {
                finish({Error::Rejected, {}, {}, {}}); return;
            }
            if (m_kind == Kind::Replace && (m_primary != m_previous || selectionFor(m_primary, raw) != m_expected)) {
                finish({Error::Rejected, {}, {}, {}}); return;
            }
            m_record = {QUuid::createUuid().toRfc4122(), fingerprint(m_token), false};
            writeRecord(false);
        });
    }
    void writeRecord(bool committed)
    {
        m_record.committed = committed;
        m_state->m_backend.write(CredentialPersistence::versionKey(m_key), encodeRecord(m_record), this, [this, committed](Raw raw) {
            if (raw.error != Error::None) { unavailable(); return; }
            if (committed) {
                finish({Error::None, m_token, m_record.version, {},
                        selectionFor(m_token, Raw{Error::None, encodeRecord(m_record), {}})});
            }
            else { writePrimary(); }
        });
    }
    void writePrimary()
    {
        const bool removing = m_kind == Kind::Retire || m_token.isEmpty();
        const auto complete = [this, removing](Raw raw) {
            if (raw.error != Error::None && !(removing && raw.error == Error::Missing)) { unavailable(); return; }
            if (removing) {
                m_state->m_backend.remove(CredentialPersistence::versionKey(m_key), this, [this](Raw removed) {
                    if (removed.error != Error::None && removed.error != Error::Missing) { unavailable(); return; }
                    finish({Error::Missing, {}, {}, {}});
                });
            } else {
                m_state->m_backend.read(m_key, this, [this](Raw observed) {
                    if (observed.error != Error::None || observed.text != m_token) { unavailable(); return; }
                    writeRecord(true);
                });
            }
        };
        if (removing) { m_state->m_backend.remove(m_key, this, complete); }
        else { m_state->m_backend.write(m_key, m_token, this, complete); }
    }
    void unavailable() { finish({Error::Unavailable, {}, {}, QStringLiteral("credential persistence could not be confirmed")}); }
    void finish(Value value)
    {
        if (m_finished) { return; }
        m_finished = true;
        m_wait.stop();
        m_lease.reset();
        const std::shared_ptr<State> state = m_state;
        Done done = std::move(m_done);
        state->released(this);
        deleteLater();
        done(std::move(value));
    }
    std::shared_ptr<State> m_state;
    Kind m_kind;
    QString m_key;
    QString m_token;
    QByteArray m_expected;
    QString m_previous;
    QString m_primary;
    Record m_record;
    Done m_done;
    Lease m_lease;
    QElapsedTimer m_elapsed;
    QTimer m_wait;
    bool m_head{false};
    bool m_started{false};
    bool m_finished{false};
};

void CredentialPersistence::State::enqueue(Kind kind, const QString& key, const QString& token, const QByteArray& version,
                                          Done done, const QString& previous)
{
    if (!done) { return; }
    if (!QCoreApplication::instance()) {
        done({Error::Unavailable, {}, {}, QStringLiteral("credential persistence requires an application")}); return;
    }
    if (!m_backend.read || !m_backend.write || !m_backend.remove || !m_acquire
        || key.isEmpty() || key.toUtf8().size() > 2048 || token.toUtf8().size() > kTokenLimit
        || (kind == Kind::Retire && (version.size() != 16 || token.isEmpty()))
        || (kind == Kind::Replace && (version.size() != 32 || previous.isEmpty() || previous.toUtf8().size() > kTokenLimit || token.isEmpty()))
        || m_outstanding >= kQueueLimit || m_queues.value(key).size() >= kEntryQueueLimit) {
        QTimer::singleShot(0, QCoreApplication::instance(), [done = std::move(done)] {
            done({Error::Unavailable, {}, {}, QStringLiteral("credential persistence request refused")});
        });
        return;
    }
    auto* operation = new Operation(shared_from_this(), kind, key, token, version, std::move(done), previous);
    ++m_outstanding;
    QList<Operation*>& queue = m_queues[key];
    queue.append(operation);
    if (queue.size() == 1) { operation->activate(); }
}

void CredentialPersistence::State::released(Operation* operation)
{
    const QString key = operation->key();
    QList<Operation*>& queue = m_queues[key];
    const bool wasHead = !queue.isEmpty() && queue.front() == operation;
    queue.removeOne(operation);
    --m_outstanding;
    if (queue.isEmpty()) { m_queues.remove(key); }
    else if (wasHead) { QTimer::singleShot(0, queue.front(), [next = queue.front()] { next->activate(); }); }
}

CredentialPersistence::CredentialPersistence(Backend backend, Acquire acquire, int admissionTimeoutMs)
    : m_state(std::make_shared<State>(std::move(backend), std::move(acquire), admissionTimeoutMs)) {}
CredentialPersistence::~CredentialPersistence() = default;
void CredentialPersistence::read(const QString& key, Done done) { m_state->enqueue(State::Kind::Read, key, {}, {}, std::move(done)); }
void CredentialPersistence::write(const QString& key, const QString& token, Done done) { m_state->enqueue(State::Kind::Write, key, token, {}, std::move(done)); }
void CredentialPersistence::retire(const QString& key, const QString& token, const QByteArray& version, Done done) { m_state->enqueue(State::Kind::Retire, key, token, version, std::move(done)); }
void CredentialPersistence::compareAndWrite(const QString& key, const QString& expectedToken, const QByteArray& selection,
                                          const QString& replacement, Done done)
{
    m_state->enqueue(State::Kind::Replace, key, replacement, selection, std::move(done), expectedToken);
}
QString CredentialPersistence::versionKey(const QString& key)
{
    return QStringLiteral("TailnetShim/Version/") + QString::fromLatin1(fingerprint(key).toHex());
}
CredentialPersistence& CredentialPersistence::system()
{
    static CredentialPersistence persistence(systemBackend(), acquireSystemLease);
    return persistence;
}
CredentialPersistence::Acquire CredentialPersistence::systemAuthority() { return acquireSystemLease; }
CredentialPersistence::Backend CredentialPersistence::keychainBackend() { return systemBackend(); }
} // namespace AetherSDR::tailnetshim
