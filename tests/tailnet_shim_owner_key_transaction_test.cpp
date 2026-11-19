#include "core/TailnetShimOwnerKeyTransaction.h"
#include "AetherPDTestTransport.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

namespace {
using Transaction = AetherSDR::TailnetShimOwnerKeyTransaction;
using Entry = Transaction::Entry;
const QString kSerial = QStringLiteral("OWNER-TEST");
const QString kInstallation(64, u'c'), kOwner(64, u'a'), kCandidate(64, u'b');
const QString kCandidateId = kSerial + u'/' + kInstallation;
int failures = 0;
void check(bool value, const char* reason)
{
    if (!value) { ++failures; std::cerr << "FAIL: " << reason << '\n'; std::exit(1); }
}

// The subject is our coordinator. This injected storage contract never
// contacts a keyring; the production persistence state machine has separate tests.
struct Storage : std::enable_shared_from_this<Storage> {
    struct Item { QString token; quint64 revision{0}; bool durable{false}; bool missing{true}; };
    QHash<QString, Item> values;
    QString paused, failWrite;
    bool available{true}, applyFailure{false}, failRetirement{false}, badCandidateReadback{false};
    int generated{0}, writes{0}, retirements{0}, candidateReads{0};
    bool held{false};
    struct Held {
        explicit Held(std::shared_ptr<Storage> value) : owner(std::move(value)) {}
        ~Held() { owner->held = false; }
        std::shared_ptr<Storage> owner;
    };
    std::function<void()> pending;
    static QString key(const QString& id, Entry entry) { return QString::number(int(entry)) + u':' + id; }
    Item& active() { return values[key(kSerial, Entry::TailnetShim)]; }
    Item& candidate() { return values[key(kCandidateId, Entry::AetherPDOwnerCandidate)]; }
    void release() { const std::function<void()> action = std::exchange(pending, {}); if (action) { action(); } }
    void schedule(const QString& label, std::function<void()> action)
    {
        if (paused == label) { check(!pending, "only one storage operation may be pending"); pending = std::move(action); }
        else { action(); }
    }
    Transaction::Acquire acquire()
    {
        const std::shared_ptr<Storage> self = shared_from_this();
        return [self](const QString&) -> Transaction::Lease {
            if (self->held) { return {}; }
            self->held = true;
            return std::make_shared<Held>(self);
        };
    }
    Transaction::Credentials operations()
    {
        const std::shared_ptr<Storage> self = shared_from_this();
        Transaction::Credentials ops;
        ops.available = [self] { return self->available; };
        ops.revision = [self](const QString& id, Entry entry) { return self->values.value(key(id, entry)).revision; };
        ops.durable = [self](const QString& id, const QString& token, quint64 revision, Entry entry) {
            const Item item = self->values.value(key(id, entry));
            return item.durable && !token.isEmpty() && item.token == token && item.revision == revision;
        };
        ops.load = [self](const QString& id, QObject* context, std::function<void(const QString&)> done, Entry entry) {
            const QPointer<QObject> guard(context);
            self->schedule(entry == Entry::TailnetShim ? QStringLiteral("load-active") : QStringLiteral("load-candidate"),
                [self, id, entry, guard, done = std::move(done)] {
                if (!guard) { return; }
                QString token = self->values[key(id, entry)].token;
                if (entry == Entry::AetherPDOwnerCandidate && ++self->candidateReads > 1 && self->badCandidateReadback) {
                    token = QString(64, u'd');
                }
                done(token);
            });
        };
        const auto write = [self](bool creating, const QString& id, const QString& expected, quint64 revision,
                                   const QString& token, QObject* context, Transaction::Done done, Entry entry) {
            Item& item = self->values[key(id, entry)];
            if (item.revision != revision || (creating ? !item.missing || !item.token.isEmpty() : item.token != expected)) { return false; }
            const quint64 writing = ++item.revision;
            const QString label = entry == Entry::TailnetShim ? QStringLiteral("promote") : QStringLiteral("save-candidate");
            ++self->writes;
            const QPointer<QObject> guard(context);
            self->schedule(label, [self, id, entry, writing, token, label, guard, done = std::move(done)] {
                Item& current = self->values[key(id, entry)];
                const bool matched = current.revision == writing;
                const bool failed = self->failWrite == label;
                if (matched && (!failed || self->applyFailure)) {
                    current.token = token; current.missing = false; current.durable = !failed; ++current.revision;
                }
                if (guard) { done(matched && !failed, {}); }
            });
            return true;
        };
        ops.create = [write](const QString& id, quint64 revision, const QString& token, QObject* context, Transaction::Done done, Entry entry) {
            return write(true, id, {}, revision, token, context, std::move(done), entry);
        };
        ops.replace = [write](const QString& id, const QString& expected, quint64 revision, const QString& token,
                               QObject* context, Transaction::Done done, Entry entry) {
            return write(false, id, expected, revision, token, context, std::move(done), entry);
        };
        ops.retire = [self](const QString& id, const QString& expected, quint64 revision, Entry entry,
                            QObject* context, std::function<void(bool)> done) {
            Item& item = self->values[key(id, entry)];
            if (item.token != expected || item.revision != revision || !item.durable) { return false; }
            ++self->retirements;
            const QPointer<QObject> guard(context);
            self->schedule(QStringLiteral("retire"), [self, id, expected, revision, entry, guard, done = std::move(done)] {
                Item& item = self->values[key(id, entry)];
                const bool removed = !self->failRetirement && item.token == expected && item.revision == revision;
                if (removed) { item.token.clear(); item.durable = false; item.missing = true; ++item.revision; }
                if (guard) { done(removed); }
            });
            return true;
        };
        return ops;
    }
};

struct Fixture {
    std::shared_ptr<Storage> storage = std::make_shared<Storage>();
    std::unique_ptr<AetherPDTest::Network> network = std::make_unique<AetherPDTest::Network>();
    std::unique_ptr<Transaction> transaction;
    std::shared_ptr<bool> current = std::make_shared<bool>(true);
    Transaction::Target target;
    int completions{0};
    Transaction::Outcome outcome{Transaction::Outcome::Unconfirmed};
    Transaction::Reason reason{Transaction::Reason::None};
    explicit Fixture(int deadlineMs = 30000)
    {
        storage->active() = {kOwner, 1, true, false};
        target.serial = kSerial; target.address = QHostAddress(QStringLiteral("192.168.50.200"));
        target.status.provisioned = true; target.status.installationId = kInstallation;
        target.status.managedOwnerVersion = 1; target.status.ownerKeyVersion = 1;
        target.sessionCurrent = [live = current] { return *live; };
        restart(deadlineMs);
    }
    void restart(int deadlineMs = 30000)
    {
        transaction.reset(); completions = 0;
        transaction = std::make_unique<Transaction>(*network, storage->operations(), storage->acquire(),
            [store = storage] { ++store->generated; return kCandidate; }, nullptr, deadlineMs);
        QObject::connect(transaction.get(), &Transaction::finished, transaction.get(),
            [this](Transaction::Outcome value, Transaction::Reason why) { ++completions; outcome = value; reason = why; });
    }
    ~Fixture() { transaction.reset(); storage->release(); }
    void confirm()
    {
        const QByteArray hash = QCryptographicHash::hash(kCandidate.toLatin1(), QCryptographicHash::Sha256).toHex();
        network->last->complete(200, QJsonDocument(QJsonObject{{"state", "owner_key_current"},
            {"installation_id", kInstallation}, {"owner_key_sha256", QString::fromLatin1(hash)}}).toJson());
    }
    void checkSuccess()
    {
        check(completions == 1 && outcome == Transaction::Outcome::Completed && reason == Transaction::Reason::None,
              "completed transaction reports verified adoption and cleanup once");
        check(storage->active().token == kCandidate && storage->active().durable && storage->candidate().token.isEmpty()
              && !storage->held, "completion retains durable active owner and removes only its candidate");
    }
};
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    {
        Fixture f;
        f.storage->paused = QStringLiteral("save-candidate");
        check(f.transaction->begin(f.target, Transaction::Action::Replace), "explicit replacement admitted");
        check(f.network->sent.empty() && f.storage->held, "no HTTP precedes the candidate's completed persistent save");
        f.storage->release();
        check(f.network->sent.size() == 1 && f.storage->candidate().durable && f.storage->candidateReads == 2
              && f.storage->active().token == kOwner, "saved candidate is read back before the single owner request");
        f.confirm(); f.checkSuccess();
    }
    for (bool applied : {false, true}) {
        Fixture f; f.storage->failWrite = QStringLiteral("save-candidate"); f.storage->applyFailure = applied;
        f.transaction->begin(f.target, Transaction::Action::Replace);
        check(f.completions == 1 && f.network->sent.empty() && f.storage->active().token == kOwner
              && f.storage->retirements == 0, "candidate save uncertainty never sends a mutation or deletes a credential");
    }
    {
        Fixture f; f.storage->badCandidateReadback = true;
        f.transaction->begin(f.target, Transaction::Action::Replace);
        check(f.network->sent.empty() && f.storage->candidate().token == kCandidate && f.storage->retirements == 0,
              "mismatched readback preserves the candidate and blocks HTTP");
    }
    for (const QString& stage : {QStringLiteral("session"), QStringLiteral("revision"), QStringLiteral("cancel")}) {
        Fixture f;
        QObject::connect(f.transaction.get(), &Transaction::stateChanged, f.transaction.get(), [&](Transaction::State state) {
            if (state != Transaction::State::Replacing) { return; }
            if (stage == u"session") { *f.current = false; }
            else if (stage == u"revision") { ++f.storage->active().revision; }
            else { f.transaction->cancel(); }
        });
        f.transaction->begin(f.target, Transaction::Action::Replace);
        check(f.network->sent.empty() && f.storage->candidate().token == kCandidate && f.storage->retirements == 0,
              "reentrant retirement or changed credential blocks the impending HTTP mutation");
    }
    for (int code : {0, 401, 503}) {
        Fixture f; f.transaction->begin(f.target, Transaction::Action::Replace);
        f.network->last->complete(code, "{}", code == 0 ? QNetworkReply::RemoteHostClosedError : QNetworkReply::NoError);
        check(f.completions == 1 && f.outcome == Transaction::Outcome::Unconfirmed && f.network->sent.size() == 1
              && f.storage->active().token == kOwner && f.storage->candidate().token == kCandidate && f.storage->retirements == 0,
              "lost or refused initial reply retains both credentials without retry");
    }
    for (bool emptyActive : {false, true}) {
        Fixture f;
        f.storage->candidate() = {kCandidate, 1, true, false};
        if (emptyActive) { f.storage->active() = {}; }
        f.transaction->begin(f.target, Transaction::Action::Resume);
        check(f.storage->generated == 0 && f.network->sent.size() == 1
              && f.network->last->request().rawHeader("Authorization") == "Bearer " + kCandidate.toLatin1(),
              "explicit resume first authenticates with the exact already saved candidate");
        f.confirm(); f.checkSuccess();
    }
    {
        Fixture f; f.storage->candidate() = {kCandidate, 1, true, false};
        f.transaction->begin(f.target, Transaction::Action::Resume);
        const QByteArray first = f.network->sent.front().body;
        f.network->last->complete(401, "{}");
        check(f.network->sent.size() == 2 && f.network->sent.back().body == first
              && f.network->last->request().rawHeader("Authorization") == "Bearer " + kOwner.toLatin1()
              && f.storage->generated == 0, "operator resume permits one exact-candidate replay with the retained owner");
        f.network->last->complete(401, "{}");
        check(f.completions == 1 && f.network->sent.size() == 2 && f.storage->candidate().token == kCandidate,
              "a second refusal does not loop or discard the saved candidate");
    }
    {
        Fixture f; f.storage->candidate() = {kCandidate, 1, false, false};
        f.transaction->begin(f.target, Transaction::Action::Resume);
        check(f.storage->writes == 1 && f.storage->candidate().durable && f.storage->generated == 0,
              "resume conditionally repairs an interrupted candidate before HTTP");
        f.confirm(); f.checkSuccess();
    }
    {
        Fixture f; f.storage->failWrite = QStringLiteral("promote"); f.storage->applyFailure = true;
        f.transaction->begin(f.target, Transaction::Action::Replace); f.confirm();
        check(f.outcome == Transaction::Outcome::Unconfirmed && f.storage->candidate().token == kCandidate
              && f.storage->retirements == 0, "uncertain active promotion never deletes its saved candidate");
        f.storage->failWrite.clear();
        f.restart(); f.transaction->begin(f.target, Transaction::Action::Resume); f.confirm(); f.checkSuccess();
        check(f.storage->generated == 1, "restart after uncertain promotion reuses the original saved candidate");
    }
    {
        Fixture f; f.storage->failRetirement = true;
        f.transaction->begin(f.target, Transaction::Action::Replace); f.confirm();
        check(f.outcome == Transaction::Outcome::CleanupUnconfirmed && f.storage->active().durable
              && f.storage->candidate().token == kCandidate, "cleanup uncertainty is distinct from completed candidate retirement");
    }
    {
        Fixture f;
        QObject::connect(f.transaction.get(), &Transaction::stateChanged, f.transaction.get(), [&](Transaction::State state) {
            if (state == Transaction::State::Cleaning) {
                f.storage->active().token = QString(64, u'd');
                ++f.storage->active().revision;
            }
        });
        f.transaction->begin(f.target, Transaction::Action::Replace); f.confirm();
        check(f.storage->retirements == 0 && f.storage->candidate().token == kCandidate
              && f.outcome == Transaction::Outcome::Unconfirmed,
              "a credential changed at the cleanup boundary preserves the saved candidate");
    }
    for (bool deleting : {false, true}) {
        Fixture f; f.storage->paused = QStringLiteral("save-candidate");
        f.transaction->begin(f.target, Transaction::Action::Replace);
        if (deleting) { f.transaction.reset(); } else { f.transaction->cancel(); }
        check(f.storage->held, "an outstanding backend completion retains the setup lease after caller retirement");
        f.storage->release();
        check(f.network->sent.empty() && f.storage->candidate().token == kCandidate && !f.storage->held,
              "late save completion after caller retirement cannot send HTTP or lose its candidate");
    }
    {
        Fixture f; f.transaction->begin(f.target, Transaction::Action::Replace);
        f.transaction.reset(); f.confirm();
        check(f.storage->active().token == kOwner && f.storage->candidate().token == kCandidate
              && f.storage->retirements == 0, "a deleted caller cannot adopt a late HTTP confirmation");
        f.restart(); f.transaction->begin(f.target, Transaction::Action::Resume); f.confirm(); f.checkSuccess();
    }
    {
        Fixture f; f.storage->paused = QStringLiteral("promote");
        f.transaction->begin(f.target, Transaction::Action::Replace); f.confirm();
        f.transaction.reset();
        check(f.storage->held, "an outstanding promotion retains authority after caller destruction");
        f.storage->release();
        check(f.storage->active().token == kCandidate && f.storage->candidate().token == kCandidate && !f.storage->held,
              "late completed promotion retains its independent candidate for explicit recovery");
        f.storage->paused.clear(); f.restart();
        f.transaction->begin(f.target, Transaction::Action::Resume); f.confirm(); f.checkSuccess();
    }
    {
        Fixture f; f.storage->paused = QStringLiteral("save-candidate");
        f.transaction->begin(f.target, Transaction::Action::Replace);
        f.network.reset(); f.storage->release();
        check(f.outcome == Transaction::Outcome::Unconfirmed && f.storage->candidate().token == kCandidate,
              "deleted injected transport cannot be used after candidate persistence");
    }
    {
        Fixture f(1); f.transaction->begin(f.target, Transaction::Action::Replace);
        QEventLoop loop;
        QObject::connect(f.transaction.get(), &Transaction::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(100, &loop, &QEventLoop::quit); loop.exec();
        check(f.reason == Transaction::Reason::Deadline && f.network->sent.size() == 1
              && f.storage->candidate().token == kCandidate && f.storage->active().token == kOwner,
              "deadline reports uncertainty without a retry or candidate deletion");
    }
    for (const QString& refusal : {QStringLiteral("owner"), QStringLiteral("candidate"), QStringLiteral("pending"), QStringLiteral("storage")}) {
        Fixture f;
        if (refusal == u"owner") { f.storage->active() = {}; }
        if (refusal == u"pending") { f.storage->candidate() = {kCandidate, 1, true, false}; }
        if (refusal == u"storage") { f.storage->available = false; }
        f.transaction->begin(f.target, refusal == u"candidate" ? Transaction::Action::Resume : Transaction::Action::Replace);
        check(f.network->sent.empty() && f.storage->generated == 0 && f.storage->retirements == 0,
              "missing owner/candidate, existing pending attempt or unavailable storage cannot start a new rotation");
    }
    std::cout << "owner-key transaction failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
