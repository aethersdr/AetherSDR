#include "core/TailnetShimCredentialPersistence.h"
#include "core/TailnetShimTokenStore.h"
#include "core/SettingsCredentialPolicy.h"
#include "NativeOwnerKeyProbe.h"

#include <QAbstractEventDispatcher>
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QRegularExpression>
#include <QTimer>
#include <qt6keychain/keychain.h>

#ifdef Q_OS_MACOS
#include <QGuiApplication>
#endif

#include <cstdio>
#include <functional>
#include <memory>
#include <utility>

using AetherSDR::TailnetShimTokenStore;
using Persistence = AetherSDR::tailnetshim::CredentialPersistence;

namespace {
void report(const char* message)
{
    std::puts(message);
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv)
{
#ifdef Q_OS_MACOS
    // QtKeychain's Apple callbacks require the Cocoa main loop, as in its testclient.
    std::unique_ptr<QCoreApplication> application;
    if (argc == 5 && QByteArray(argv[1]) == "core-loop-absent") {
        application = std::make_unique<QCoreApplication>(argc, argv);
    } else {
        application = std::make_unique<QGuiApplication>(argc, argv);
    }
    QCoreApplication& app = *application;
#else
    QCoreApplication app(argc, argv);
#endif
    app.setApplicationName(QStringLiteral("AetherSDR-native-credential-probe"));
    std::fprintf(stderr, "qt_version=%s event_dispatcher=%s\n", qVersion(),
                 QCoreApplication::eventDispatcher()->metaObject()->className());
    if (argc != 5) { return 2; }
    const QString mode = QString::fromLocal8Bit(argv[1]);
    const QString id = QString::fromLocal8Bit(argv[2]);
    const QString value = QString::fromLocal8Bit(argv[3]);
    const QString mailbox = QString::fromLocal8Bit(argv[4]);
    const QRegularExpression identifier(QStringLiteral("\\Aaetherpd-native-credential-[0-9a-f]{32}\\z"));
    if (!identifier.match(id).hasMatch()) { return 2; }
    const QString key = QStringLiteral("TailnetShim/AdminToken/") + id;
    const QString candidateId = id + QStringLiteral("/owner-rotation-fixture");
    const QString candidate = QStringLiteral("1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");
    using Entry = TailnetShimTokenStore::Entry;
    const auto finish = [&app](bool ok) {
        report(ok ? "pass" : "fail");
        app.exit(ok ? 0 : 1);
    };
    QTimer::singleShot(15000, &app, [&app] {
        report("deadline");
        app.exit(3);
    });

    // Start after exec(): synchronous refusals must also produce an exit code.
    QTimer::singleShot(0, &app, [&] {
        if (mode == u"owner-cleanup" || mode == u"owner-transaction-cleanup") {
            const QString cleaning = mode == u"owner-cleanup" ? candidateId : id + u'/' + QString(64, u'c');
            TailnetShimTokenStore::save(cleaning, {}, &app, [&, finish](bool persisted, const QString&) {
                if (!persisted) { finish(false); return; }
                TailnetShimTokenStore::save(id, {}, &app, [finish](bool saved, const QString&) { finish(saved); });
            }, Entry::AetherPDOwnerCandidate);
        } else if (mode == u"owner-candidate-absent" || mode == u"owner-candidate-absent-meta"
                   || mode == u"owner-transaction-absent" || mode == u"owner-transaction-absent-meta") {
            const QString checking = mode.startsWith(u"owner-transaction-") ? id + u'/' + QString(64, u'c') : candidateId;
            QString slot = AetherSDR::SettingsCredentialPolicy::scopedCredentialKey(
                AetherSDR::SettingsCredentialPolicy::kAetherPDOwnerCandidate,
                QString::fromLatin1(QCryptographicHash::hash(checking.toUtf8(), QCryptographicHash::Sha256).toHex()));
            if (mode.endsWith(u"-meta")) { slot = Persistence::versionKey(slot); }
            auto* job = new QKeychain::ReadPasswordJob(QStringLiteral("AetherSDR"), &app);
            job->setKey(slot);
            job->setAutoDelete(true);
            job->setInsecureFallback(false);
            QObject::connect(job, &QKeychain::Job::finished, &app, [job, finish](QKeychain::Job*) {
                finish(job->error() == QKeychain::EntryNotFound);
            });
            job->start();
        } else if (mode == u"owner-transaction" || mode == u"owner-transaction-lost" || mode == u"owner-transaction-missing") {
            runNativeOwnerKeyProbe(app, mode, id, finish);
        } else if (mode == u"owner-promote" || mode == u"owner-promote-close") {
            TailnetShimTokenStore::save(candidateId, candidate, &app, [&, finish](bool persisted, const QString&) {
                if (!persisted) { finish(false); return; }
                TailnetShimTokenStore::load(candidateId, &app, [&, finish](const QString& saved) {
                    if (saved != candidate || !TailnetShimTokenStore::isDurableCurrent(candidateId, saved,
                            TailnetShimTokenStore::revision(candidateId, Entry::AetherPDOwnerCandidate), Entry::AetherPDOwnerCandidate)) {
                        finish(false); return;
                    }
                    TailnetShimTokenStore::load(id, &app, [&, finish](const QString& current) {
                        if (current != value) { finish(false); return; }
                        const quint64 revision = TailnetShimTokenStore::revision(id);
                        if (mode == u"owner-promote-close") {
                            QObject context;
                            if (!TailnetShimTokenStore::replaceIfCurrent(id, value, revision, candidate, &context, {})) {
                                finish(false); return;
                            }
                            // The context is gone before the backend completes.
                            Persistence::system().read(key, [&, finish](Persistence::Value result) {
                                finish(result.error == Persistence::Error::None && result.token == candidate
                                       && result.version.size() == 16 && result.selection.size() == 32);
                            });
                            return;
                        }
                        const bool admitted = TailnetShimTokenStore::replaceIfCurrent(id, value, revision, candidate, &app,
                            [&, finish](bool promoted, const QString&) {
                                if (!promoted) { finish(false); return; }
                                TailnetShimTokenStore::load(id, &app, [&, finish](const QString& active) {
                                    if (active != candidate) { finish(false); return; }
                                    TailnetShimTokenStore::load(candidateId, &app, [&, finish](const QString& retained) {
                                        finish(retained == candidate);
                                    }, Entry::AetherPDOwnerCandidate);
                                });
                            });
                        if (!admitted) { finish(false); }
                    });
                }, Entry::AetherPDOwnerCandidate);
            }, Entry::AetherPDOwnerCandidate);
        } else if (mode == u"write" || mode == u"cleanup") {
            TailnetShimTokenStore::save(id, mode == u"cleanup" ? QString{} : value, &app,
                [finish](bool persisted, const QString&) { finish(persisted); });
        } else if (mode == u"legacy-write") {
            auto* job = new QKeychain::WritePasswordJob(QStringLiteral("AetherSDR"), &app);
            job->setKey(key);
            job->setTextData(value);
            job->setInsecureFallback(false);
            job->setAutoDelete(true);
            QObject::connect(job, &QKeychain::Job::finished, &app, [job, finish](QKeychain::Job*) {
                finish(job->error() == QKeychain::NoError);
            });
            job->start();
        } else if (mode == u"pending-write" || mode == u"inflight-read-save") {
            struct Completion { bool written{false}; bool loaded{false}; bool valid{false}; };
            const auto state = std::make_shared<Completion>();
            const auto complete = [state, finish] {
                if (state->written && state->loaded) { finish(state->valid); }
            };
            const auto read = [&, state, complete] {
                TailnetShimTokenStore::load(id, &app, [state, complete, value](const QString& token) {
                    state->loaded = true;
                    state->valid = token == value;
                    complete();
                });
            };
            if (mode == u"inflight-read-save") { read(); }
            TailnetShimTokenStore::save(id, value, &app, [state, complete, finish](bool persisted, const QString&) {
                if (!persisted) { finish(false); return; }
                state->written = true;
                complete();
            });
            if (mode == u"pending-write") { read(); }
        } else if (mode == u"read") {
            TailnetShimTokenStore::load(id, &app, [finish, value](const QString& token) { finish(token == value); });
        } else if (mode == u"raw-read" || mode == u"raw-metadata" || mode == u"core-loop-absent"
                   || mode == u"absent" || mode == u"absent-metadata") {
            auto* job = new QKeychain::ReadPasswordJob(QStringLiteral("AetherSDR"), &app);
            job->setKey(mode == u"raw-metadata" || mode == u"absent-metadata" ? Persistence::versionKey(key) : key);
            job->setAutoDelete(true);
            job->setInsecureFallback(false);
            QObject::connect(job, &QKeychain::Job::finished, &app, [job, mode, value, finish](QKeychain::Job*) {
                if (mode == u"absent" || mode == u"absent-metadata" || mode == u"core-loop-absent") {
                    finish(job->error() == QKeychain::EntryNotFound);
                } else if (mode == u"raw-read") {
                    finish(job->error() == QKeychain::NoError && job->textData() == value);
                } else {
                    finish(job->error() == QKeychain::NoError && !job->textData().isEmpty());
                }
            });
            job->start();
        } else if (mode == u"write-close") {
            QObject context;
            TailnetShimTokenStore::save(id, value, &context);
            Persistence::system().read(key, [value, finish](Persistence::Value read) {
                finish(read.error == Persistence::Error::None && read.token == value && read.version.size() == 16);
            });
        } else if (mode == u"busy-write") {
            const auto transaction = std::make_shared<Persistence>(Persistence::keychainBackend(), Persistence::systemAuthority(), 100);
            transaction->write(key, value, [transaction, finish](Persistence::Value result) {
                finish(result.error == Persistence::Error::Unavailable);
            });
        } else if (mode == u"phase-before" || mode == u"phase-after") {
            Persistence::Backend backend = Persistence::keychainBackend();
            const auto realWrite = backend.write;
            backend.write = [&, realWrite](const QString& item, const QString& text, QObject* owner, Persistence::RawDone done) {
                if (item != key) { realWrite(item, text, owner, std::move(done)); return; }
                if (mode == u"phase-before") {
                    report("phase_paused_before_primary");
                } else {
                    realWrite(item, text, owner, [](Persistence::Raw raw) {
                        if (raw.error == Persistence::Error::None) { report("phase_paused_after_primary"); }
                        else { report("phase_backend_failed"); }
                    });
                }
            };
            const auto transaction = std::make_shared<Persistence>(backend, Persistence::systemAuthority());
            transaction->write(key, value, [transaction, finish](Persistence::Value result) {
                finish(result.error == Persistence::Error::None);
            });
        } else if (mode == u"actor" || mode == u"owner-actor" || mode == u"lease-holder") {
            struct Actor {
                quint64 revision{0};
                Persistence::Lease lease;
            };
            const auto state = std::make_shared<Actor>();
            auto* input = new QTimer(&app);
            input->setInterval(10);
            if (mode == u"lease-holder") {
                state->lease = Persistence::systemAuthority()(key);
                if (!state->lease) { finish(false); return; }
                input->start();
                report("lease_held");
            } else {
                const auto load = [&, state, input, finish] {
                    TailnetShimTokenStore::load(id, &app, [state, input, value, id, finish](const QString& token) {
                        if (token != value) { finish(false); return; }
                        state->revision = TailnetShimTokenStore::revision(id);
                        input->start();
                        report("actor_loaded");
                    });
                };
                if (mode == u"owner-actor") {
                    TailnetShimTokenStore::save(candidateId, candidate, &app,
                        [load, finish](bool saved, const QString&) { if (saved) { load(); } else { finish(false); } },
                        Entry::AetherPDOwnerCandidate);
                } else { load(); }
            }
            // A private atomic mailbox works on Windows too; stdin is not a socket.
            QObject::connect(input, &QTimer::timeout, &app, [&, input, state, finish] {
                QFile file(mailbox);
                if (!file.exists()) { return; }
                input->stop();
                if (!file.open(QIODevice::ReadOnly) || file.size() > 16) { finish(false); return; }
                const QByteArray command = file.readAll();
                file.close();
                if (!file.remove()) { finish(false); return; }
                if (command == "promote" && mode == u"owner-actor") {
                    const bool admitted = TailnetShimTokenStore::replaceIfCurrent(id, value, state->revision, candidate, &app,
                        [finish](bool promoted, const QString&) {
                            report(promoted ? "promotion_unexpectedly_succeeded" : "promotion_rejected");
                            finish(!promoted);
                        });
                    if (!admitted) { finish(false); }
                } else if (command == "reload") {
                    TailnetShimTokenStore::load(id, &app, [input, value](const QString& token) {
                        report(token == value ? "reload_same_token" : "reload_changed_token");
                        input->start();
                    });
                } else if (command == "release") {
                    state->lease.reset();
                    report("lease_released");
                    app.exit(0);
                } else if (command == "retire") {
                    const bool admitted = TailnetShimTokenStore::clearIfCurrent(id, value, state->revision,
                        TailnetShimTokenStore::Entry::TailnetShim, &app, [&app](bool removed) {
                            report(removed ? "retirement_completed" : "retirement_refused");
                            app.exit(removed ? 0 : 4);
                        });
                    if (!admitted) { report("retirement_refused"); app.exit(4); }
                } else { finish(false); }
            });
        } else { app.exit(2); }
    });
    return app.exec();
}
