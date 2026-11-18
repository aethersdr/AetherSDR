#include "core/TailnetShimCredentialPersistence.h"
#include "core/TailnetShimTokenStore.h"

#include <QAbstractEventDispatcher>
#include <QByteArray>
#include <QCoreApplication>
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
        if (mode == u"write" || mode == u"cleanup") {
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
        } else if (mode == u"actor" || mode == u"lease-holder") {
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
                TailnetShimTokenStore::load(id, &app, [state, input, value, id, finish](const QString& token) {
                    if (token != value) { finish(false); return; }
                    state->revision = TailnetShimTokenStore::revision(id);
                    input->start();
                    report("actor_loaded");
                });
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
                if (command == "reload") {
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
