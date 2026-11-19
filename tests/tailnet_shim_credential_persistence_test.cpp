#include "core/TailnetShimCredentialPersistence.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QEvent>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>

namespace {
using Store = AetherSDR::tailnetshim::CredentialPersistence;
using Error = Store::Error;
using Value = Store::Value;
const QString kKey = QStringLiteral("fixture-primary");

bool check(bool value, const char* message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; }
    return value;
}

struct Memory : std::enable_shared_from_this<Memory> {
    QHash<QString, QString> values;
    bool held{false};
    bool leaseViolation{false};
    int calls{0};
    int failAt{0};
    int pauseAt{0};
    bool applyOnFailure{false};
    std::function<void()> pending;
    struct Held {
        explicit Held(std::shared_ptr<Memory> memory) : memory(std::move(memory)) {}
        ~Held() { memory->held = false; }
        std::shared_ptr<Memory> memory;
    };

    void invoke(QObject* owner, Store::RawDone done, std::function<Store::Raw()> action)
    {
        leaseViolation |= !held;
        const int call = ++calls;
        const QPointer<QObject> guard(owner);
        const std::shared_ptr<Memory> self = shared_from_this();
        const auto complete = [self, guard, done = std::move(done), action = std::move(action), call] {
            if (!guard) { return; }
            Store::Raw raw;
            if (call != self->failAt || self->applyOnFailure) { raw = action(); }
            if (call == self->failAt) { raw = {Error::Unavailable, {}, QStringLiteral("injected backend failure")}; }
            done(std::move(raw));
        };
        if (call == pauseAt) { pending = complete; }
        else { complete(); }
    }
    void resume()
    {
        const std::function<void()> callback = std::exchange(pending, {});
        callback();
    }
    Store::Backend backend()
    {
        const std::shared_ptr<Memory> self = shared_from_this();
        return {
            [self](const QString& key, QObject* owner, Store::RawDone done) {
                self->invoke(owner, std::move(done), [self, key] {
                    return self->values.contains(key) ? Store::Raw{Error::None, self->values.value(key), {}}
                                                      : Store::Raw{Error::Missing, {}, {}};
                });
            },
            [self](const QString& key, const QString& value, QObject* owner, Store::RawDone done) {
                self->invoke(owner, std::move(done), [self, key, value] {
                    self->values.insert(key, value); return Store::Raw{Error::None, {}, {}};
                });
            },
            [self](const QString& key, QObject* owner, Store::RawDone done) {
                self->invoke(owner, std::move(done), [self, key] {
                    return Store::Raw{self->values.remove(key) ? Error::None : Error::Missing, {}, {}};
                });
            }
        };
    }
    Store::Acquire acquire()
    {
        const std::shared_ptr<Memory> self = shared_from_this();
        return [self](const QString&) -> Store::Lease {
            if (self->held) { return {}; }
            self->held = true;
            return std::make_shared<Held>(self);
        };
    }
};

std::optional<Value> wait(const std::function<void(Store::Done)>& start)
{
    std::optional<Value> result;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    start([&](Value value) { result = std::move(value); loop.quit(); });
    if (!result) { deadline.start(1000); loop.exec(); }
    return result;
}

QString record(bool committed, const QString& token, int schema = 1)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"schema", schema}, {"version", QString(32, u'a')},
        {"sha256", QString::fromLatin1(QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256).toHex())},
        {"committed", committed}}).toJson(QJsonDocument::Compact));
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    {
        const auto memory = std::make_shared<Memory>();
        Store store(memory->backend(), memory->acquire());
        memory->values.insert(Store::versionKey(kKey), record(true, QStringLiteral("unknown"), 2));
        const auto unknown = wait([&](Store::Done done) { store.createIfMissing(kKey, QStringLiteral("verified"), std::move(done)); });
        ok &= check(unknown && unknown->error == Error::Unavailable && !memory->values.contains(kKey),
                    "conditional creation refuses unknown orphaned metadata");
        memory->values.remove(Store::versionKey(kKey));
        const auto created = wait([&](Store::Done done) { store.createIfMissing(kKey, QStringLiteral("verified"), std::move(done)); });
        const auto refused = wait([&](Store::Done done) { store.createIfMissing(kKey, QStringLiteral("other"), std::move(done)); });
        ok &= check(created && created->error == Error::None && created->version.size() == 16 && created->selection.size() == 32
                        && refused && refused->error == Error::Rejected && memory->values.value(kKey) == QStringLiteral("verified")
                        && !memory->leaseViolation && !memory->held,
                    "explicit creation admits only an absent primary and cannot overwrite a competing value");
    }
    for (int boundary = 1; boundary <= 6; ++boundary) {
        for (bool apply : {false, true}) {
            const auto memory = std::make_shared<Memory>();
            Store store(memory->backend(), memory->acquire());
            memory->failAt = boundary; memory->applyOnFailure = apply;
            const auto failed = wait([&](Store::Done done) { store.createIfMissing(kKey, QStringLiteral("verified"), std::move(done)); });
            ok &= check(failed && failed->error == Error::Unavailable && !memory->held,
                        "conditional creation reports uncertainty at every backend boundary");
            memory->failAt = 0;
            const auto observed = wait([&](Store::Done done) { store.read(kKey, std::move(done)); });
            const auto recovered = wait([&](Store::Done done) {
                if (observed->error == Error::Missing) {
                    store.createIfMissing(kKey, QStringLiteral("verified"), std::move(done));
                } else {
                    store.compareAndWrite(kKey, observed->token, observed->selection, QStringLiteral("verified"), std::move(done));
                }
            });
            ok &= check(recovered && recovered->error == Error::None && memory->values.value(kKey) == QStringLiteral("verified"),
                        "explicit recovery handles both absent and whole-primary interrupted creation states");
        }
    }
    {
        const auto memory = std::make_shared<Memory>();
        Store a(memory->backend(), memory->acquire());
        Store b(memory->backend(), memory->acquire());
        const auto original = wait([&](Store::Done done) { a.write(kKey, QStringLiteral("old"), std::move(done)); });
        const auto observed = wait([&](Store::Done done) { a.read(kKey, std::move(done)); });
        const auto identical = wait([&](Store::Done done) { b.write(kKey, QStringLiteral("old"), std::move(done)); });
        ok &= check(original && observed && identical && observed->selection.size() == 32
                        && observed->selection == original->selection && identical->selection != observed->selection,
                    "conditional replacement observes both primary bytes and the committed save identity");
        const auto stale = wait([&](Store::Done done) {
            a.compareAndWrite(kKey, QStringLiteral("old"), observed->selection, QStringLiteral("candidate"), std::move(done));
        });
        ok &= check(stale && stale->error == Error::Rejected && memory->values.value(kKey) == QStringLiteral("old"),
                    "an identical-token save invalidates an earlier conditional replacement");
        const auto replaced = wait([&](Store::Done done) {
            b.compareAndWrite(kKey, QStringLiteral("old"), identical->selection, QStringLiteral("candidate"), std::move(done));
        });
        const auto current = wait([&](Store::Done done) { a.read(kKey, std::move(done)); });
        ok &= check(replaced && current && replaced->error == Error::None && current->token == QStringLiteral("candidate")
                        && replaced->selection == current->selection && replaced->version == current->version
                        && !memory->leaseViolation && !memory->held,
                    "matching conditional replacement commits and reads back under one authority lease");
        const auto wrong = wait([&](Store::Done done) {
            a.compareAndWrite(kKey, QStringLiteral("foreign"), current->selection, QStringLiteral("other"), std::move(done));
        });
        ok &= check(wrong && wrong->error == Error::Rejected && memory->values.value(kKey) == QStringLiteral("candidate"),
                    "a matching selection cannot replace a different expected primary");
    }
    for (int boundary = 1; boundary <= 6; ++boundary) {
        for (bool apply : {false, true}) {
            const auto memory = std::make_shared<Memory>();
            Store store(memory->backend(), memory->acquire());
            const auto previous = wait([&](Store::Done done) { store.write(kKey, QStringLiteral("old"), std::move(done)); });
            const auto candidate = wait([&](Store::Done done) { store.write(QStringLiteral("saved-candidate"), QStringLiteral("candidate"), std::move(done)); });
            ok &= check(previous && candidate && previous->error == Error::None && candidate->error == Error::None,
                        "the independent candidate is durable before testing active-key promotion");
            memory->calls = 0; memory->failAt = boundary; memory->applyOnFailure = apply;
            const auto failed = wait([&](Store::Done done) {
                store.compareAndWrite(kKey, QStringLiteral("old"), previous->selection, QStringLiteral("candidate"), std::move(done));
            });
            ok &= check(failed && failed->error == Error::Unavailable && !memory->held && !memory->leaseViolation
                            && memory->values.value(QStringLiteral("saved-candidate")) == QStringLiteral("candidate"),
                        "a failed promotion retains its separately saved candidate at every backend boundary");
            memory->failAt = 0;
            const auto observed = wait([&](Store::Done done) { store.read(kKey, std::move(done)); });
            ok &= check(observed && observed->error == Error::None && observed->selection.size() == 32
                            && (observed->token == QStringLiteral("old") || observed->token == QStringLiteral("candidate")),
                        "a promotion interrupted before or after effect leaves an observable whole primary");
            const auto recovered = wait([&](Store::Done done) {
                store.compareAndWrite(kKey, observed->token, observed->selection, QStringLiteral("candidate"), std::move(done));
            });
            ok &= check(recovered && recovered->error == Error::None && recovered->version.size() == 16
                            && memory->values.value(kKey) == QStringLiteral("candidate"),
                        "a fresh exact observation permits explicit recovery without an unconditional overwrite");
        }
    }
    {
        const auto memory = std::make_shared<Memory>();
        Store a(memory->backend(), memory->acquire());
        Store b(memory->backend(), memory->acquire());
        const auto previous = wait([&](Store::Done done) { a.write(kKey, QStringLiteral("old"), std::move(done)); });
        memory->calls = 0; memory->pauseAt = 2;
        std::optional<Value> first;
        a.compareAndWrite(kKey, QStringLiteral("old"), previous->selection, QStringLiteral("winner"),
                          [&](Value value) { first = std::move(value); });
        ok &= check(memory->held && memory->pending && !first, "comparison retains authority until publication completes");
        const auto second = wait([&](Store::Done done) {
            b.compareAndWrite(kKey, QStringLiteral("old"), previous->selection, QStringLiteral("loser"), std::move(done));
            ok &= check(memory->calls == 2 && !first, "competing promotion cannot enter the first comparison interval");
            memory->resume();
        });
        ok &= check(first && second && first->error == Error::None && second->error == Error::Rejected
                        && memory->values.value(kKey) == QStringLiteral("winner") && !memory->leaseViolation,
                    "a queued competing promotion cannot overwrite the first committed candidate");
    }
    {
        const auto memory = std::make_shared<Memory>();
        Store store(memory->backend(), memory->acquire());
        const auto invalid = wait([&](Store::Done done) {
            store.compareAndWrite(kKey, QStringLiteral("old"), QByteArray(31, 'x'), QStringLiteral("new"), std::move(done));
        });
        ok &= check(invalid && invalid->error == Error::Unavailable && memory->calls == 0,
                    "invalid conditional proof refuses before backend access");
        const auto missing = wait([&](Store::Done done) {
            store.compareAndWrite(kKey, QStringLiteral("old"), QByteArray(32, 'x'), QStringLiteral("new"), std::move(done));
        });
        ok &= check(missing && missing->error == Error::Rejected && memory->values.isEmpty(),
                    "conditional promotion cannot create a missing active owner entry");
    }
    {
        const auto memory = std::make_shared<Memory>();
        Store a(memory->backend(), memory->acquire());
        Store b(memory->backend(), memory->acquire());
        const auto first = wait([&](Store::Done done) { a.write(kKey, QStringLiteral("fixture-token"), std::move(done)); });
        const auto second = wait([&](Store::Done done) { b.write(kKey, QStringLiteral("fixture-token"), std::move(done)); });
        ok &= check(first && second && first->error == Error::None && second->error == Error::None
                        && first->version.size() == 16 && first->version != second->version
                        && memory->values.value(kKey) == QStringLiteral("fixture-token"), "identical token saves have distinct versions and retain the raw primary format");
        const auto refused = wait([&](Store::Done done) { a.retire(kKey, QStringLiteral("fixture-token"), first->version, std::move(done)); });
        ok &= check(refused && refused->error == Error::Rejected && memory->values.value(kKey) == QStringLiteral("fixture-token"),
                    "a stale version cannot delete an identical-token replacement");
        const auto removed = wait([&](Store::Done done) { b.retire(kKey, QStringLiteral("fixture-token"), second->version, std::move(done)); });
        ok &= check(removed && removed->error == Error::Missing && memory->values.isEmpty() && !memory->held && !memory->leaseViolation,
                    "matching retirement removes the primary and sidecar under one lease");
    }
    {
        const auto memory = std::make_shared<Memory>();
        memory->values.insert(kKey, QStringLiteral("legacy-token"));
        Store store(memory->backend(), memory->acquire());
        const auto legacy = wait([&](Store::Done done) { store.read(kKey, std::move(done)); });
        const auto refused = wait([&](Store::Done done) { store.retire(kKey, QStringLiteral("legacy-token"), QByteArray(16, 'a'), std::move(done)); });
        ok &= check(legacy && legacy->error == Error::None && legacy->token == QStringLiteral("legacy-token") && legacy->version.isEmpty()
                        && refused && refused->error == Error::Rejected && memory->values.value(kKey) == QStringLiteral("legacy-token"),
                    "unversioned legacy credentials remain readable and cannot authorize conditional deletion");
    }
    for (bool committed : {false, true}) {
        const auto memory = std::make_shared<Memory>();
        memory->values.insert(kKey, QStringLiteral("usable-primary"));
        memory->values.insert(Store::versionKey(kKey), record(committed, committed ? QStringLiteral("other-token") : QStringLiteral("usable-primary")));
        Store store(memory->backend(), memory->acquire());
        const auto read = wait([&](Store::Done done) { store.read(kKey, std::move(done)); });
        const auto refused = wait([&](Store::Done done) { store.retire(kKey, QStringLiteral("usable-primary"), QByteArray(16, char(0xaa)), std::move(done)); });
        ok &= check(read && read->error == Error::None && read->token == QStringLiteral("usable-primary") && read->version.isEmpty()
                        && refused && refused->error == Error::Rejected && memory->values.value(kKey) == QStringLiteral("usable-primary"),
                    "interrupted or mismatched metadata preserves the primary without authorizing retirement");
    }
    for (const QString& invalid : {QStringLiteral("{}"), record(true, QStringLiteral("old"), 2), QString(513, u'x'),
                                   record(true, QStringLiteral("old")).replace(QString(32, u'a'), QString(32, u'A'))}) {
        const auto memory = std::make_shared<Memory>();
        memory->values.insert(kKey, QStringLiteral("old")); memory->values.insert(Store::versionKey(kKey), invalid);
        Store store(memory->backend(), memory->acquire());
        const auto read = wait([&](Store::Done done) { store.read(kKey, std::move(done)); });
        const auto write = wait([&](Store::Done done) { store.write(kKey, QStringLiteral("new"), std::move(done)); });
        ok &= check(read && write && read->error == Error::Unavailable && write->error == Error::Unavailable
                        && memory->values.value(kKey) == QStringLiteral("old") && memory->values.value(Store::versionKey(kKey)) == invalid,
                    "unknown, malformed, oversized and noncanonical metadata is preserved and refused");
    }
    for (int failAt = 1; failAt <= 5; ++failAt) {
        for (bool apply : {false, true}) {
            const auto memory = std::make_shared<Memory>();
            memory->values.insert(kKey, QStringLiteral("old")); memory->failAt = failAt; memory->applyOnFailure = apply;
            Store store(memory->backend(), memory->acquire());
            const auto failed = wait([&](Store::Done done) { store.write(kKey, QStringLiteral("new"), std::move(done)); });
            ok &= check(failed && failed->error == Error::Unavailable && !memory->held && !memory->leaseViolation,
                        "failure at each persistence boundary releases authority only after the backend outcome");
            memory->failAt = 0;
            const auto observed = wait([&](Store::Done done) { store.read(kKey, std::move(done)); });
            ok &= check(observed && observed->error == Error::None
                            && (observed->token == QStringLiteral("old") || observed->token == QStringLiteral("new")),
                        "failed writes, including errors after effect, retain the last atomic primary value");
            const auto recovered = wait([&](Store::Done done) { store.write(kKey, QStringLiteral("recovered"), std::move(done)); });
            ok &= check(recovered && recovered->error == Error::None && recovered->version.size() == 16,
                        "an explicit retry can commit after any interrupted stage");
        }
    }
    {
        const auto memory = std::make_shared<Memory>();
        Store a(memory->backend(), memory->acquire(), 15); Store b(memory->backend(), memory->acquire(), 30);
        memory->pauseAt = 2;
        std::optional<Value> written;
        a.write(kKey, QStringLiteral("pending"), [&](Value value) { written = std::move(value); });
        ok &= check(memory->pending && memory->held && !written, "a backend-pending write retains authority");
        const auto busy = wait([&](Store::Done done) { b.read(kKey, std::move(done)); });
        ok &= check(busy && busy->error == Error::Unavailable && memory->calls == 2 && memory->held,
                    "another authority times out without starting a backend operation or releasing the writer lease");
        memory->resume();
        ok &= check(written && written->error == Error::None && !memory->held, "late backend completion safely finishes the retained write");
    }
    {
        const auto memory = std::make_shared<Memory>();
        Store a(memory->backend(), memory->acquire()); Store b(memory->backend(), memory->acquire());
        const auto original = wait([&](Store::Done done) { a.write(kKey, QStringLiteral("old"), std::move(done)); });
        memory->calls = 0; memory->pauseAt = 2;
        std::optional<Value> retired; std::optional<Value> replaced;
        a.retire(kKey, QStringLiteral("old"), original->version, [&](Value value) { retired = std::move(value); });
        b.write(kKey, QStringLiteral("new"), [&](Value value) { replaced = std::move(value); });
        ok &= check(memory->held && memory->calls == 2 && !retired && !replaced,
                    "a competing save cannot enter the comparison/deletion interval");
        memory->resume();
        QEventLoop loop; QTimer timer; timer.setInterval(5);
        QObject::connect(&timer, &QTimer::timeout, &loop, [&] { if (replaced) { loop.quit(); } });
        QTimer::singleShot(1000, &loop, &QEventLoop::quit); timer.start(); if (!replaced) { loop.exec(); }
        ok &= check(retired && replaced && retired->error == Error::Missing && replaced->error == Error::None
                        && memory->values.value(kKey) == QStringLiteral("new") && !memory->leaseViolation,
                    "a save ordered after retirement survives its deletion");
    }
    {
        const auto memory = std::make_shared<Memory>();
        Store store(memory->backend(), memory->acquire());
        const auto empty = wait([&](Store::Done done) { store.read({}, std::move(done)); });
        const auto key = wait([&](Store::Done done) { store.read(QString(2049, u'x'), std::move(done)); });
        const auto token = wait([&](Store::Done done) { store.write(kKey, QString(4097, u'x'), std::move(done)); });
        const auto proof = wait([&](Store::Done done) { store.retire(kKey, QStringLiteral("token"), QByteArray(17, 'x'), std::move(done)); });
        ok &= check(empty && key && token && proof && empty->error == Error::Unavailable && key->error == Error::Unavailable
                        && token->error == Error::Unavailable && proof->error == Error::Unavailable && memory->calls == 0,
                    "invalid identities, oversized values and invalid versions refuse before backend access");
    }
    {
        const auto memory = std::make_shared<Memory>();
        memory->pauseAt = 2;
        Store store(memory->backend(), memory->acquire(), 25);
        std::optional<Value> written;
        store.write(kKey, QStringLiteral("pending"), [&](Value value) { written = std::move(value); });
        int answered = 0;
        bool allUnavailable = true;
        for (int index = 0; index < 24; ++index) {
            store.read(kKey, [&](Value value) { ++answered; allUnavailable &= value.error == Error::Unavailable; });
        }
        QEventLoop loop; QTimer timer; timer.setInterval(5);
        QObject::connect(&timer, &QTimer::timeout, &loop, [&] { if (answered == 24) { loop.quit(); } });
        QTimer::singleShot(1000, &loop, &QEventLoop::quit); timer.start(); if (answered != 24) { loop.exec(); }
        ok &= check(answered == 24 && allUnavailable && memory->calls == 2 && memory->held && !written,
                    "queue admission stays bounded while an in-flight backend retains its lease beyond the admission deadline");
        memory->resume();
        ok &= check(written && written->error == Error::None && !memory->held, "backpressure never cancels the active write");
    }
    {
        const auto memory = std::make_shared<Memory>();
        memory->pauseAt = 2;
        std::optional<Value> written;
        {
            Store temporary(memory->backend(), memory->acquire());
            temporary.write(kKey, QStringLiteral("retained"), [&](Value value) { written = std::move(value); });
        }
        ok &= check(memory->held && !written, "destroying the persistence facade does not release an active backend lease");
        memory->resume();
        ok &= check(written && written->error == Error::None && !memory->held, "the application-owned operation completes after facade destruction");
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return ok ? 0 : 1;
}
