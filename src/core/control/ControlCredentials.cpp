#include "ControlCredentials.h"

#include <QCryptographicHash>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QSet>
#include <QThread>
#include <QUuid>

namespace AetherSDR::control {
namespace {
bool validId(const QString& id)
{
    if (id.size() != 32) { return false; }
    for (const QChar c : id) {
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f'))) { return false; }
    }
    return true;
}

bool sameDigest(const QByteArray& a, const QByteArray& b)
{
    // Fixed-size SHA-256 comparisons. Volatile accumulation avoids a compiler
    // replacing this with an early-exit string comparison. No secret prefix
    // controls the loop length, and verify examines every bounded record.
    if (a.size() != 32 || b.size() != 32) { return false; }
    volatile unsigned difference = 0;
    for (qsizetype i = 0; i < 32; ++i) {
        difference = difference | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    }
    return difference == 0;
}
} // namespace

struct ControlCredentials::State {
    QString id;
    Role role;
    QByteArray digest;
    std::atomic<bool> retired{false};
};

bool ControlCredentials::Principal::valid() const
{
    return m_state && !m_state->retired.load(std::memory_order_acquire);
}

QString ControlCredentials::Principal::id() const
{
    return valid() ? m_state->id : QString{};
}

bool ControlCredentials::Principal::canAdministerGrants() const
{
    return valid() && m_state->role == Role::GrantAdmin;
}

ControlCredentials::ControlCredentials(QObject* parent) : QObject(parent) {}

ControlCredentials::~ControlCredentials()
{
    m_closing = true;
    for (const auto& record : m_records) { record->retired.store(true, std::memory_order_release); }
    emit changed();
}

bool ControlCredentials::onThread() const
{
    return QThread::currentThread() == thread();
}

ControlCredentials::Record ControlCredentials::generate(Role role)
{
    if (role != Role::Client && role != Role::GrantAdmin) { return {}; }
    Record result;
    result.id = QUuid::createUuid().toString(QUuid::Id128);
    result.role = role;
    result.secret.resize(kSecretBytes);
    for (char& byte : result.secret) {
        byte = static_cast<char>(QRandomGenerator::system()->generate() & 0xff);
    }
    return result;
}

bool ControlCredentials::validRecords(const QList<Record>& records)
{
    QSet<QString> ids;
    QSet<QByteArray> digests;
    if (records.size() > kMaximumRecords) { return false; }
    for (const Record& record : records) {
        if (!validId(record.id) || record.secret.size() != kSecretBytes
            || (record.role != Role::Client && record.role != Role::GrantAdmin)
            || ids.contains(record.id)) { return false; }
        const QByteArray digest = QCryptographicHash::hash(record.secret, QCryptographicHash::Sha256);
        if (digests.contains(digest)) { return false; }
        ids.insert(record.id);
        digests.insert(digest);
    }
    return true;
}

bool ControlCredentials::replace(const QList<Record>& records)
{
    if (!onThread() || m_changing || m_closing) { return false; }
    QList<std::shared_ptr<State>> next;
    const bool valid = validRecords(records);
    if (valid) {
        for (const Record& record : records) {
            auto state = std::make_shared<State>();
            state->id = record.id;
            state->role = record.role;
            state->digest = QCryptographicHash::hash(record.secret, QCryptographicHash::Sha256);
            next.append(std::move(state));
        }
    }
    m_changing = true;
    const QPointer<ControlCredentials> self(this);
    const auto restore = qScopeGuard([self] { if (self) { self->m_changing = false; } });
    for (const auto& record : m_records) { record->retired.store(true, std::memory_order_release); }
    m_records = valid ? std::move(next) : QList<std::shared_ptr<State>>{};
    emit changed();
    return self && valid;
}

bool ControlCredentials::revoke(const QString& id)
{
    if (!onThread() || m_changing || m_closing) { return false; }
    for (qsizetype i = 0; i < m_records.size(); ++i) {
        if (m_records[i]->id != id) { continue; }
        m_changing = true;
        const QPointer<ControlCredentials> self(this);
        const auto restore = qScopeGuard([self] { if (self) { self->m_changing = false; } });
        m_records[i]->retired.store(true, std::memory_order_release);
        m_records.removeAt(i);
        emit changed();
        return true;
    }
    return false;
}

void ControlCredentials::clear()
{
    (void)replace({});
}

ControlCredentials::Principal ControlCredentials::verify(const QJsonValue& auth) const
{
    if (!onThread() || m_changing || m_closing || !auth.isObject()) { return {}; }
    const QJsonObject object = auth.toObject();
    if (object.size() != 2 || object.value(QStringLiteral("scheme")) != QJsonValue(QStringLiteral("bearer"))
        || !object.value(QStringLiteral("token")).isString()) { return {}; }
    const QString token = object.value(QStringLiteral("token")).toString();
    if (token.size() != 2 * kSecretBytes) { return {}; }
    for (const QChar c : token) {
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f'))) { return {}; }
    }
    const QByteArray digest = QCryptographicHash::hash(QByteArray::fromHex(token.toLatin1()),
                                                     QCryptographicHash::Sha256);
    Principal result;
    for (const auto& record : m_records) {
        if (sameDigest(digest, record->digest) && !record->retired.load(std::memory_order_acquire)) {
            result.m_state = record;
            result.m_source = const_cast<ControlCredentials*>(this);
        }
    }
    return result;
}

qsizetype ControlCredentials::size() const
{
    return onThread() ? m_records.size() : 0;
}

} // namespace AetherSDR::control
