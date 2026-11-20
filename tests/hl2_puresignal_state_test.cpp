// PureSignal preferences and link recovery through the real backend, with no
// transport start, socket, RF or TX audio. Separate child processes prove disk
// persistence; injected link edges exercise the production restore ordering.
#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/MetisClient.h"
#include "gui/PureSignalApplet.h"
#include "SeamThreadAffinityProbe.h"
#include "TestSettingsProfile.h"

#include <QApplication>
#include <QEvent>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QSettings>

#include <cstdio>
#include <memory>

namespace AetherSDR::hl2 {
struct Hl2PureSignalBackendTestAccess {
    static void prepare(Hl2Backend& b, const QString& serial)
    {
        b.m_radioSerial = serial;
        b.m_boardMaxRx = 4;
        b.m_sampleRateHz = 48000;
        b.m_txAllowed = true; // policy input only; no keying call or transport
        b.loadPureSignalPreferences();
    }
    static void edge(Hl2Backend& b, const char* name)
    {
        QMetaObject::invokeMethod(b.m_metis, name, Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&b, QEvent::MetaCall);
    }
    static void drain(Hl2Backend& b)
    {
        QMetaObject::invokeMethod(b.m_pureSignal, [] {}, Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(&b, QEvent::MetaCall);
    }
    static QVariantMap fields(const Hl2Backend& b) { return b.pureSignalFields(); }
    static bool wanted(const Hl2Backend& b) { return b.m_psWanted; }
    static int attenuation(const Hl2Backend& b) { return b.m_psAttenuationDb; }
    static bool controllerKeyed(Hl2Backend& b)
    {
        bool keyed = true;
        QObject::connect(b.m_pureSignal, &Hl2PureSignal::statusChanged, b.m_pureSignal,
                         [&keyed](const Hl2PureSignalStatus& s) { keyed = s.keyed; }, Qt::DirectConnection);
        QMetaObject::invokeMethod(b.m_pureSignal, [&] {
            b.m_pureSignal->setAvailability(false, QStringLiteral("test"));
        }, Qt::BlockingQueuedConnection);
        QObject::disconnect(b.m_pureSignal, nullptr, b.m_pureSignal, nullptr);
        return keyed;
    }
    static void staleKey(Hl2Backend& b)
    {
        b.m_keyed = true;
        QMetaObject::invokeMethod(b.m_pureSignal, "setKeyed", Qt::BlockingQueuedConnection,
                                  Q_ARG(bool, true));
    }
};
}

using namespace AetherSDR;
using hl2::Hl2Backend;
using Access = hl2::Hl2PureSignalBackendTestAccess;
namespace {
int failures = 0;
void check(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
bool enabled(const Hl2Backend& b) { return Access::fields(b).value("enabled").toBool(); }
void command(Hl2Backend& b, const QString& verb, const QVariant& arg)
{
    b.invokeExtension(QStringLiteral("hl2"), QStringLiteral("puresignal.") + verb, 1, arg);
}
void persistenceChild(bool write)
{
    Hl2Backend b;
    test::SeamThreadAffinityProbe probe(&b);
    test::attachAllSeamSignals(probe);
    Access::prepare(b, QStringLiteral("PS-TEST-A"));
    Access::edge(b, "linkUp");
    if (write) {
        command(b, QStringLiteral("enable"), true);
        check(enabled(b), "operator enables PS");
        command(b, QStringLiteral("autoatt"), false);
        command(b, QStringLiteral("att"), -20);
        command(b, QStringLiteral("autoatt"), true);
        check(Access::wanted(b), "enable preference captured");
    } else {
        check(enabled(b) && Access::wanted(b), "separate process restores enabled from disk at linkUp");
        check(Access::attenuation(b) == 31, "auto-attenuate starts safely at 31, ignoring saved manual -20");
        Access::drain(b);
        bool disconnected = false;
        int lateStatus = 0;
        QVariantMap lastFields;
        QObject lifecycleObserver;
        QObject::connect(&b, &IRadioBackend::disconnected, &lifecycleObserver, [&] {
            check(lastFields.contains("available") && !lastFields.value("available").toBool()
                  && !lastFields.value("enabled").toBool(), "final unavailable PS status precedes disconnected");
            disconnected = true;
        });
        QObject::connect(&b, &IRadioBackend::extensionStatus, &lifecycleObserver,
                         [&](const QString&, const QString&, const QVariantMap& fields) {
                             lastFields = fields;
                             if (disconnected) { ++lateStatus; }
                         });
        Access::edge(b, "linkDown");
        Access::drain(b);
        check(lateStatus == 0, "queued PureSignal status cannot emit after disconnected");
        disconnected = false;
        check(!enabled(b) && Access::wanted(b), "watchdog off preserves operator preference");
        Access::staleKey(b);
        Access::edge(b, "linkUp");
        check(enabled(b), "watchdog recovery restores PS");
        check(!Access::controllerKeyed(b), "linkUp clears I/O controller key before restoring PS");
        command(b, QStringLiteral("enable"), false);
        check(!Access::wanted(b), "operator off replaces persisted preference");
        Access::edge(b, "linkDown");
        Access::edge(b, "linkUp");
        check(!enabled(b), "operator off remains off on later linkUp");
    }
    check(probe.violations().isEmpty(), "backend seam signals stay on owner thread");
}
void isolatedRadioAndSchemas()
{
    Hl2Backend b;
    Access::prepare(b, QStringLiteral("PS-TEST-B"));
    Access::edge(b, "linkUp");
    check(!enabled(b) && !Access::wanted(b), "another radio does not inherit enabled");
    const RadioSettingsScope scope(QStringLiteral("hl2"), QStringLiteral("PS-TEST-B"));
    check(scope.setFeature(QStringLiteral("PureSignal"), 1,
                           {{"autoAttenuate", false}, {"attenuationDb", -20}}), "seed legacy schema 1 document");
    Access::prepare(b, QStringLiteral("PS-TEST-B"));
    check(!Access::wanted(b), "schema 1 without enabled defaults off");
    check(Access::attenuation(b) == -20, "manual attenuation survives load");
    check(scope.setFeature(QStringLiteral("PureSignal"), 2, {{"future", true}}), "seed newer schema");
    Access::prepare(b, QStringLiteral("PS-TEST-B"));
    command(b, QStringLiteral("enable"), true);
    int version = 0;
    const QJsonObject doc = scope.featureExact(QStringLiteral("PureSignal"), &version);
    check(version == 2 && doc.value("future").toBool() && !doc.contains("enabled"),
          "operator changes do not overwrite a newer schema");
}
void fileErrorView()
{
    Hl2Backend b;
    PureSignalApplet applet;
    applet.setBackend(&b);
    for (int error : {1, 2}) {
        emit b.extensionStatus(QStringLiteral("hl2"), QStringLiteral("puresignal"),
                               {{"available", true}, {"enabled", true}, {"correcting", true},
                                {"fileError", error}});
        bool shown = false;
        for (const QLabel* label : applet.findChildren<QLabel*>()) {
            shown = shown || label->text().contains(error == 2
                ? QStringLiteral("restore error") : QStringLiteral("save error"));
            check(!label->text().contains(QStringLiteral("Correction held")), "file error overrides ready status");
        }
        check(shown, "WDSP save/restore error reaches visible applet label");
    }
}
}
int main(int argc, char** argv)
{
    const bool child = argc > 1;
    std::unique_ptr<TestSettingsProfile> profile;
    if (!child) { profile = std::make_unique<TestSettingsProfile>(QStringLiteral("hl2-ps-state")); }
    // Keep first-run legacy migration isolated in child processes too.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromUtf8(qgetenv("AETHER_SETTINGS_DIR")) + "/legacy");
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("hl2-puresignal-state-test"));
    AppSettings::instance().load();
    if (child) {
        persistenceChild(QString::fromLocal8Bit(argv[1]) == QStringLiteral("--write"));
    } else {
        check(profile->isValid(), "private settings directory exists");
        for (const QString& stage : {QStringLiteral("--write"), QStringLiteral("--read")}) {
            QProcess process;
            process.start(QCoreApplication::applicationFilePath(), {stage});
            check(process.waitForFinished(30000), "persistence child completes");
            std::fputs(process.readAllStandardError().constData(), stderr);
            check(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
                  "persistence child passes");
        }
        isolatedRadioAndSchemas();
        fileErrorView();
    }
    if (!failures) { std::puts("hl2_puresignal_state_test: all checks passed"); }
    return failures ? 1 : 0;
}
