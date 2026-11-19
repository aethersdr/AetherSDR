#include "NativeOwnerKeyProbe.h"
#include "AetherPDTestTransport.h"
#include "core/TailnetShimOwnerKeyTransaction.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <memory>
#include <utility>

namespace {
using Transaction = AetherSDR::TailnetShimOwnerKeyTransaction;
using Store = AetherSDR::TailnetShimTokenStore;
const QString kOwner(64, u'a'), kCandidate(64, u'b'), kInstallation(64, u'c');

// Real production keychain and coordinator; only our own HTTP API is injected.
class Probe final : public QObject {
public:
    Probe(QCoreApplication& app, QString mode, QString id, std::function<void(bool)> finish)
        : QObject(&app), m_mode(std::move(mode)), m_id(std::move(id)), m_finish(std::move(finish))
    {
        m_candidateId = m_id + u'/' + kInstallation;
        const bool missing = m_mode == u"owner-transaction-missing";
        m_serverOwner = missing ? kCandidate : kOwner;
        Store::save(missing ? m_candidateId : m_id, missing ? kCandidate : kOwner, this,
            [this, missing](bool persisted, const QString&) {
                if (!persisted) { done(false); return; }
                begin(missing ? Transaction::Action::Resume : Transaction::Action::Replace);
            }, missing ? Store::Entry::AetherPDOwnerCandidate : Store::Entry::TailnetShim);
    }
private:
    void done(bool passed)
    {
        if (m_done) { return; }
        m_done = true;
        m_finish(passed);
    }
    void begin(Transaction::Action action)
    {
        m_transaction = std::make_unique<Transaction>(m_network, Transaction::systemCredentials(),
            AetherSDR::TailnetShimClient::acquireProvisioningLease, [] { return kCandidate; }, this);
        connect(m_transaction.get(), &Transaction::stateChanged, this, [this](Transaction::State state) {
            if (state == Transaction::State::Replacing || state == Transaction::State::Confirming) {
                QTimer::singleShot(0, this, [this] { answer(); });
            }
        });
        connect(m_transaction.get(), &Transaction::finished, this, [this](Transaction::Outcome outcome, Transaction::Reason) {
            if (m_mode == u"owner-transaction-lost" && !m_resumed) {
                if (outcome != Transaction::Outcome::Unconfirmed || m_network.sent.size() != 1) { done(false); return; }
                Store::load(m_id, this, [this](const QString& token) {
                    if (token != kOwner) { done(false); return; }
                    Store::load(m_candidateId, this, [this](const QString& candidate) {
                        if (candidate != kCandidate || !Store::isDurableCurrent(m_candidateId, candidate,
                                Store::revision(m_candidateId, Store::Entry::AetherPDOwnerCandidate), Store::Entry::AetherPDOwnerCandidate)) {
                            done(false); return;
                        }
                        m_resumed = true;
                        QTimer::singleShot(0, this, [this] { begin(Transaction::Action::Resume); });
                    }, Store::Entry::AetherPDOwnerCandidate);
                });
                return;
            }
            if (outcome != Transaction::Outcome::Completed || m_serverOwner != kCandidate
                || m_network.sent.size() != (m_resumed ? 2U : 1U)) { done(false); return; }
            Store::load(m_id, this, [this](const QString& token) {
                done(token == kCandidate && Store::isDurableCurrent(m_id, token, Store::revision(m_id)));
            });
        });
        Transaction::Target target;
        target.serial = m_id;
        target.address = QHostAddress(QStringLiteral("192.168.50.200"));
        target.status.provisioned = true;
        target.status.installationId = kInstallation;
        target.status.managedOwnerVersion = 1;
        target.status.ownerKeyVersion = 1;
        target.sessionCurrent = [] { return true; };
        if (!m_transaction->begin(target, action)) { done(false); }
    }
    void answer()
    {
        if (m_done || !m_network.last || m_network.sent.size() != ++m_answered) { done(false); return; }
        const auto& sent = m_network.sent.back();
        const QJsonObject body = QJsonDocument::fromJson(sent.body).object();
        if (sent.method != QNetworkAccessManager::PostOperation || sent.request.url().path() != u"/v1/owner-key"
            || body.size() != 2 || body.value("installation_id").toString() != kInstallation
            || body.value("new_token").toString() != kCandidate
            || sent.request.rawHeader("Authorization") != "Bearer " + m_serverOwner.toLatin1()
            || !Store::isDurableCurrent(m_candidateId, kCandidate,
                Store::revision(m_candidateId, Store::Entry::AetherPDOwnerCandidate), Store::Entry::AetherPDOwnerCandidate)) {
            done(false); return;
        }
        m_serverOwner = kCandidate;
        if (m_mode == u"owner-transaction-lost" && !m_resumed) {
            m_network.last->complete(0, {}, QNetworkReply::RemoteHostClosedError);
        } else {
            const QByteArray hash = QCryptographicHash::hash(kCandidate.toLatin1(), QCryptographicHash::Sha256).toHex();
            m_network.last->complete(200, QJsonDocument(QJsonObject{{"state", "owner_key_current"},
                {"installation_id", kInstallation}, {"owner_key_sha256", QString::fromLatin1(hash)}}).toJson());
        }
    }
    QString m_mode, m_id, m_candidateId, m_serverOwner;
    std::function<void(bool)> m_finish;
    AetherPDTest::Network m_network;
    std::unique_ptr<Transaction> m_transaction;
    size_t m_answered{0};
    bool m_resumed{false}, m_done{false};
};
}
void runNativeOwnerKeyProbe(QCoreApplication& app, const QString& mode, const QString& identifier,
                            std::function<void(bool)> finish)
{
    // The application owns the probe and every asynchronous credential operation.
    new Probe(app, mode, identifier, std::move(finish));
}
