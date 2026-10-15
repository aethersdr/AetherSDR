// Default run is socket-free. Opt-in --bridge-proof serves the production
// agent automation bridge over an owner-only local socket (no radio transport).
// #6437 regression: published FlexLib rates, production request
// admission, applet restore and replacement. No firmware peer or RF operation.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AutomationServer.h"
#include "core/ThemeManager.h"
#include "core/TciServer.h"
#include "core/TciClient.h"
#include "core/TciProtocol.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/SliceDelta.h"
#include "gui/DaxIqApplet.h"
#include "models/ModelCapabilities.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QBrush>
#include <QAbstractItemView>
#include <QComboBox>
#include <QEventLoop>
#include <QPushButton>
#include <QProgressBar>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTimer>
#include <QPixmap>
#include <QStringList>
#include <functional>
#include <memory>
#include <cstdio>

using namespace AetherSDR;

#ifdef HAVE_WEBSOCKETS
namespace AetherSDR {
// Uses the existing friend test accessor; the injected TciClient has no socket.
class TciServerReviewTest {
public:
    static bool deferredRate(RadioModel& model, const std::function<void(const QString&)>& changeModel)
    {
        changeModel(QString());
        TciServer server(&model);
        TciClient client;
        QStringList sent;
        client.textSink = [&sent](const QString& text) { sent.append(text); return text.size(); };
        TciProtocol protocol(&model, &server.m_routingState, &server.m_trxMap);
        TciServer::ClientState cs;
        cs.socket = &client;
        cs.protocol = &protocol;
        cs.iqReceivers.insert(0);
        server.m_clients.append(cs);
        QObject::connect(&client, &TciClient::textMessageReceived,
                         &server, &TciServer::onTextMessage);
        SliceDelta slice;
        slice.mode = QStringLiteral("USB");
        slice.panId = QStringLiteral("0x40000000");
        model.emitBackendSliceChangedForTest(0, slice);
        server.wireSlice(0, model.slice(0));
        QSignalSpy commands(&model.daxIqModel(), &DaxIqModel::commandReady);
        bool ok = true;
        for (const int rate : {96000, 192000}) {
            emit client.textMessageReceived(QStringLiteral("iq_samplerate:%1;").arg(rate));
            ok = ok && server.m_iqSampleRate == rate
                && !sent.isEmpty() && sent.last() == QStringLiteral("iq_samplerate:%1;").arg(rate);
        }
        emit client.textMessageReceived(QStringLiteral("iq_samplerate:44100;"));
        ok = ok && server.m_iqSampleRate == 192000 && commands.isEmpty()
            && !server.ensureIqStream(0);
        changeModel(QStringLiteral("FLEX-6400M"));
        emit model.capabilitiesChanged(true, model.backendCapabilities());
        ok = ok && server.m_iqSampleRate == 192000
            && server.achievedIqSampleRate() == 96000 && server.ensureIqStream(0)
            && sent.contains(QStringLiteral("iq_samplerate:96000;"));
        commands.clear();
        model.daxIqModel().applyStreamStatus(0x110, {{"daxiq_channel", "1"}, {"daxiq_rate", "48000"}});
        ok = ok && commands.size() == 1
            && commands.first().first().toString().endsWith(QStringLiteral("daxiq_rate=96000"));
        model.daxIqModel().handleDisconnect();
        server.resetIqStreamBookkeeping();
        changeModel(QStringLiteral("FLEX-6700"));
        emit model.capabilitiesChanged(true, model.backendCapabilities());
        ok = ok && server.m_iqSampleRate == 192000
            && server.achievedIqSampleRate() == 192000 && server.ensureIqStream(0)
            && sent.last() == QStringLiteral("iq_samplerate:192000;");
        changeModel(QStringLiteral("FLEX-6400M"));
        emit model.capabilitiesChanged(true, model.backendCapabilities());
        emit client.textMessageReceived(QStringLiteral("iq_samplerate:96000;"));
        sent.clear();
        emit client.textMessageReceived(QStringLiteral("iq_samplerate:192000;"));
        ok = ok && server.m_iqSampleRate == 96000 && sent.size() == 1
            && sent.last() == QStringLiteral("iq_samplerate:96000;")
            && protocol.generateInitBurst().contains(QStringLiteral("iq_samplerate:96000;"));
        server.m_clients.clear();
        return ok;
    }

    static bool rateReplacement(RadioModel& model, const std::function<void(const QString&)>& changeModel)
    {
        TciServer server(&model);
        TciClient client;
        QStringList sent;
        client.textSink = [&sent](const QString& text) { sent.append(text); return text.size(); };
        TciProtocol protocol(&model, &server.m_routingState, &server.m_trxMap);
        TciServer::ClientState cs;
        cs.socket = &client;
        cs.protocol = &protocol;
        cs.iqReceivers.insert(0);
        server.m_clients.append(cs);
        QObject::connect(&client, &TciClient::textMessageReceived,
                         &server, &TciServer::onTextMessage);
        SliceDelta slice;
        slice.mode = QStringLiteral("USB");
        slice.panId = QStringLiteral("0x40000000");
        model.emitBackendSliceChangedForTest(0, slice);
        server.wireSlice(0, model.slice(0));
        emit client.textMessageReceived(QStringLiteral("iq_samplerate:192000;"));
        bool ok = server.m_iqSampleRate == 192000 && server.ensureIqStream(0);
        model.daxIqModel().handleDisconnect();
        server.resetIqStreamBookkeeping();
        changeModel(QStringLiteral("FLEX-6400"));
        sent.clear();
        server.announceEffectiveIqSampleRate();
        ok = ok && server.m_iqSampleRate == 192000
            && server.achievedIqSampleRate() == 96000
            && server.ensureIqStream(0)
            && sent.contains(QStringLiteral("iq_samplerate:96000;"))
            && protocol.generateInitBurst().contains(QStringLiteral("iq_samplerate:96000;"));
        emit client.textMessageReceived(QStringLiteral("iq_samplerate:192000;"));
        ok = ok && sent.last() == QStringLiteral("iq_samplerate:96000;");
        QSignalSpy requests(&model.daxIqModel(), &DaxIqModel::commandReady);
        model.daxIqModel().applyStreamStatus(0x100, {{"daxiq_channel","1"}, {"daxiq_rate","48000"}});
        ok = ok && model.daxIqModel().stream(1).rateSettling
            && requests.size() == 1 && requests.first().first().toString().endsWith("96000");
        model.daxIqModel().handleDisconnect();
        server.resetIqStreamBookkeeping();
        changeModel(QStringLiteral("FLEX-6700"));
        sent.clear();
        server.announceEffectiveIqSampleRate();
        ok = ok && server.achievedIqSampleRate() == 192000
            && server.ensureIqStream(0)
            && sent.contains(QStringLiteral("iq_samplerate:192000;"));
        emit client.textMessageReceived(QStringLiteral("iq_samplerate:96000;"));
        ok = ok && server.m_iqSampleRate == 96000;
        changeModel(QString());
        ok = ok && !server.ensureIqStream(0);
        server.m_clients.clear();
        return ok;
    }
};
}
#endif


namespace {
int failures = 0;
void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}
void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
class RateBackend final : public IRadioBackend {
public:
    QString model{QStringLiteral("FLEX-6700")};
    bool connected{true};
    RadioCapabilities capabilities() const override {
        RadioCapabilities caps;
        caps.hasDaxStreams = true;
        caps.daxIqSampleRates = capabilitiesFor(model).daxIqSampleRates();
        return caps;
    }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};
void announce(RadioModel& model, RateBackend& backend)
{
    emit model.capabilitiesChanged(backend.connected, backend.capabilities());
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("dax-iq-rate-limits"));
    qputenv("XDG_RUNTIME_DIR", profile.path().toUtf8());
    QApplication app(argc, argv);
    AppSettings::instance().load();

    // Test the real Flex capability publisher, including changing its identity.
    QString name = QStringLiteral("FLEX-6700");
    FlexBackend flex;
    flex.setModelProvider([&name] { return name; });
    check(flex.capabilities().daxIqSampleRates == QList<int>{24000,48000,96000,192000},
          "Flex publishes 192 kHz for FLEX-6700");
    name = QStringLiteral("FLEX-6400M");
    check(flex.capabilities().daxIqSampleRates == QList<int>{24000,48000,96000},
          "Flex publishes revised 96 kHz limit");
    name.clear();
    check(flex.capabilities().daxIqSampleRates.isEmpty(), "unknown Flex model offers no IQ rates");

    RadioModel model;
    auto backend = std::make_unique<RateBackend>();
    RateBackend* source = backend.get();
    model.setBackendForTest(std::move(backend), QStringLiteral("test"));
    DaxIqModel& iq = model.daxIqModel();
    if (app.arguments().contains(QStringLiteral("--bridge-proof"))) {
        const QString token = qEnvironmentVariable("AETHER_MCP_TOKEN");
        if (token.isEmpty()) {
            std::fprintf(stderr, "--bridge-proof requires a non-empty AETHER_MCP_TOKEN\n");
            return 1;
        }
        AppSettings& settings = AppSettings::instance();
        settings.setValue("DaxIqRate1", "192000");
        settings.setValue("DaxIqEnabled1", "False");
        settings.save();
        QWidget window;
        window.setObjectName(QStringLiteral("daxIqRateProof"));
        window.setWindowTitle(QStringLiteral("DAX IQ model limit proof — no radio transport"));
        auto* layout = new QVBoxLayout(&window);
        auto* applet = new DaxIqApplet(&window);
        applet->setObjectName(QStringLiteral("daxIqProofApplet"));
        layout->addWidget(applet);
        source->model = QStringLiteral("FLEX-6400M");
        applet->setRadioModel(&model);
        QObject::connect(applet, &DaxIqApplet::iqRateChanged, &iq, &DaxIqModel::setSampleRate);
        for (const char* name : {"FLEX-6400M", "FLEX-6700"}) {
            auto* button = new QPushButton(QString::fromLatin1(name), &window);
            button->setObjectName(QString::fromLatin1(name));
            layout->addWidget(button);
            QObject::connect(button, &QPushButton::clicked, &window, [&model, source, name] {
                model.daxIqModel().handleDisconnect();
                source->model = QString::fromLatin1(name);
                announce(model, *source);
            });
        }
        auto* quit = new QPushButton(QStringLiteral("Close proof"), &window);
        quit->setObjectName(QStringLiteral("closeProof"));
        layout->addWidget(quit);
        QObject::connect(quit, &QPushButton::clicked, &app, &QCoreApplication::quit);
        applet->show();
        window.show();
        AutomationServer bridge;
        bridge.setRadioModel(&model);
        bridge.setAuthToken(token);
        qputenv("AETHER_AUTOMATION_NO_TX", "1");
        if (!bridge.start(qEnvironmentVariable("AETHER_AUTOMATION_SOCKET"))) {
            return 1;
        }
        bridge.setTxAllowed(false);
        QTimer::singleShot(60000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    QSignalSpy commands(&iq, &DaxIqModel::commandReady);
    auto status = [&iq](quint32 id, int ch, int rate) {
        iq.applyStreamStatus(id, {{"daxiq_channel",QString::number(ch)},
                                 {"daxiq_rate",QString::number(rate)}});
    };
    for (const int rate : {24000, 48000, 96000, 192000}) {
        check(iq.supportsSampleRate(rate), "all FLEX-6700 IQ rates admitted");
        iq.setSampleRate(1, rate);
        status(0x100, 1, 48000);
        commands.clear();
        iq.setSampleRate(1, rate);
        check(commands.size() == 1 && commands.at(0).at(0).toString().endsWith(QString::number(rate)),
              "supported request emits exact rate");
    }
    commands.clear();
    for (const int rate : {-1, 0, 44100, 97000, 384000}) {
        iq.setSampleRate(1, rate);
    }
    check(commands.isEmpty(), "malformed or unlisted rates emit no command");
    iq.handleDisconnect();
    source->model = QStringLiteral("FLEX-6300");
    commands.clear();
    iq.setSampleRate(1,192000);
    check(commands.isEmpty(), "96 kHz model rejects pre-create 192 kHz request");
    status(0x101,1,48000);
    check(commands.size() == 1 && commands.at(0).at(0).toString().endsWith("96000"),
          "new session clamps previously accepted 192 kHz intent before reapply");
    commands.clear();
    iq.setSampleRate(1,192000);
    check(commands.isEmpty(), "live stream rejects 192 kHz on limited model");
    for (const int rate : {24000,48000,96000}) {
        iq.setSampleRate(1,rate);
    }
    check(commands.size() == 3, "limited model accepts each supported request");
    iq.handleDisconnect();
    source->model.clear();
    commands.clear();
    iq.createStream(2);
    iq.setSampleRate(2,48000);
    status(0x102,2,24000);
    check(commands.isEmpty(), "unknown model cannot create or reapply an IQ rate");
    iq.handleDisconnect();

#ifdef HAVE_WEBSOCKETS
    check(TciServerReviewTest::deferredRate(model, [source](const QString& name) {
        source->model = name;
    }), "TCI defers pre-identification rates without commands, then clamps and restores intent");
    iq.handleDisconnect();
    source->model = QStringLiteral("FLEX-6700");
    check(TciServerReviewTest::rateReplacement(model, [source](const QString& name) {
        source->model = name;
    }), "TCI retains 192k preference, restores at 96k, refuses unsupported SET and announces effective rate");
    iq.handleDisconnect();
#endif

    AppSettings& settings = AppSettings::instance();
    settings.setValue("DaxIqRate1", "192000");
    settings.setValue("DaxIqEnabled1", "True");
    settings.save();
    source->model = QStringLiteral("FLEX-6400M");
    DaxIqApplet applet;
    QSignalSpy rates(&applet, &DaxIqApplet::iqRateChanged);
    QSignalSpy enables(&applet, &DaxIqApplet::iqEnableRequested);
    applet.setRadioModel(&model);
    auto* combo = applet.findChild<QComboBox*>("daxIqRate1");
    auto* items = qobject_cast<QStandardItemModel*>(combo->model());
    check(combo->count() == 4 && !items->item(3)->isEnabled(),
          "192k remains visible but disabled on limited model");
    check(combo->itemData(3,Qt::ForegroundRole).value<QBrush>().color()
              == ThemeManager::instance().color(combo, "color.control.unavailable"),
          "disabled choice uses the theme unavailable color");
    check(!combo->itemData(3,Qt::AccessibleDescriptionRole).toString().isEmpty()
              && combo->accessibleDescription().contains("192"),
          "disabled choice has an accessible reason");
    check(combo->currentData().toInt() == 96000, "saved 192k displays safe 96k fallback");
    if (app.arguments().contains(QStringLiteral("--capture"))) {
        QTemporaryDir captureDir;
        if (!captureDir.isValid()) {
            return 1;
        }
        captureDir.setAutoRemove(false);
        std::printf("Capture directory: %s\n", qPrintable(captureDir.path()));
        applet.resize(280,120);
        applet.show();
        app.processEvents();
        applet.grab().save(captureDir.filePath(QStringLiteral("dax-iq-96k-applet.png")));
        combo->showPopup();
        app.processEvents();
        combo->view()->grab().save(captureDir.filePath(QStringLiteral("dax-iq-96k-choices.png")));
        combo->hidePopup();
    }
    check(enables.wait(2500), "restore completes within the deadline");
    check(rates.size() == 1 && rates.at(0).at(1).toInt() == 96000 && enables.size() == 1,
          "restore requests safe fallback exactly once");
    check(settings.value("DaxIqRate1").toInt() == 192000, "fallback leaves valid saved preference intact");
    combo->setCurrentIndex(3); // programmatic invocation can select a disabled row
    check(rates.size() == 1 && combo->currentData().toInt() == 96000,
          "programmatic unsupported selection is refused and restored");

    source->model = QStringLiteral("FLEX-6700");
    announce(model,*source);
    check(items->item(3)->isEnabled() && combo->currentData().toInt() == 192000,
          "model revision re-enables 192k and restores preference");
    source->connected = false;
    emit model.connectionStateChanged(false);
    spin(2500);
    check(enables.size() == 1, "disconnect cancels pending restore");
    source->connected = true;
    emit model.connectionStateChanged(true);
    check(enables.wait(2500), "reconnect restore completes within the deadline");
    check(rates.last().at(1).toInt() == 192000 && enables.size() == 2,
          "reconnect restores valid 192k selection");

    RadioModel replacement;
    auto other = std::make_unique<RateBackend>();
    RateBackend* otherSource = other.get();
    otherSource->model = QStringLiteral("AU-510M");
    replacement.setBackendForTest(std::move(other),QStringLiteral("test"));
    iq.setSampleRate(1, 192000);
    iq.applyStreamStatus(0x201, {{"daxiq_channel", "1"}, {"daxiq_rate", "192000"},
                               {"pan", "0x40000000"}});
    applet.setDaxIqLevel(1, 32768.0f);
    check(applet.findChild<QPushButton*>("daxIqEnable1")->text() == "On",
          "old model starts with a confirmed live IQ channel");
    bool oldMeterActive = false;
    for (QProgressBar* meter : applet.findChildren<QProgressBar*>()) {
        oldMeterActive = oldMeterActive || meter->value() > 0;
    }
    check(oldMeterActive, "old model has an IQ level to clear on replacement");
    check(!applet.findChild<QPushButton*>("daxIqEnable1")->accessibleDescription()
              .contains(QStringLiteral("not currently active")),
          "confirmed On stream is announced as active");
    applet.restoreEnabledChannels();
    spin(1000);
    applet.setRadioModel(&replacement);
    check(applet.findChild<QPushButton*>("daxIqEnable1")->text() == "Off",
          "replacement without streams immediately clears old On state");
    bool allMetersZero = true;
    for (QProgressBar* meter : applet.findChildren<QProgressBar*>()) {
        allMetersZero = allMetersZero && meter->value() == 0;
    }
    check(allMetersZero, "replacement immediately clears old IQ meter levels");
    rates.clear();
    enables.clear();
    announce(model,*source);
    emit model.connectionStateChanged(true);
    emit iq.streamChanged(1);
    spin(600);
    check(enables.isEmpty(), "old restore timer cannot fire early in replacement session");
    check(enables.wait(2500), "replacement restore completes within the deadline");
    check(rates.size() == 1 && rates.at(0).at(1).toInt() == 96000 && enables.size() == 1,
          "replacement ignores old model signals and cancels its pending timer");
    check(settings.value("DaxIqRate1").toInt() == 192000, "replacement still preserves saved preference");
    otherSource->model.clear();
    announce(replacement,*otherSource);
    check(!combo->isEnabled() && combo->accessibleDescription().contains(QStringLiteral("until")),
          "unknown model dims rate control with its availability reason");
    applet.setRadioModel(nullptr);
    spin(2500);
    check(enables.size() == 1, "unbinding model cancels restore");

    otherSource->model = QStringLiteral("FLEX-6400M");
    DaxIqModel& replacementIq = replacement.daxIqModel();
    replacementIq.setSampleRate(2, 24000);
    replacementIq.applyStreamStatus(0x202, {{"daxiq_channel", "2"}, {"daxiq_rate", "24000"}});
    applet.setRadioModel(&replacement);
    check(applet.findChild<QPushButton*>("daxIqEnable2")->text() == "On",
          "late applet binding seeds already-confirmed stream state");
    applet.setRadioModel(nullptr);

    auto* enable = applet.findChild<QPushButton*>("daxIqEnable2");
    applet.setRadioModel(&replacement);
    check(enable->styleSheet().contains(QStringLiteral("background: #00b4d8;")),
          "late-bound On stream retains the On background");
    replacementIq.handleStreamRemoved(0x202);
    check(enable->text() == "Off"
              && enable->styleSheet().contains(QStringLiteral("background: #1a2a3a;")),
          "On to Off updates the rendered background after availability refresh");
    replacementIq.applyStreamStatus(0x202, {{"daxiq_channel", "2"}, {"daxiq_rate", "24000"}});
    check(enable->text() == "On"
              && enable->styleSheet().contains(QStringLiteral("background: #00b4d8;")),
          "Off to On updates the rendered background after availability refresh");
    applet.setRadioModel(nullptr);

    // A live rate request has not yet been echoed when a theme refresh occurs.
    replacementIq.setSampleRate(2, 24000);
    replacementIq.applyStreamStatus(0x202, {{"daxiq_rate", "24000"}});
    DaxIqApplet pendingApplet;
    pendingApplet.setRadioModel(&replacement);
    auto* pendingCombo = pendingApplet.findChild<QComboBox*>("daxIqRate2");
    QObject::connect(&pendingApplet, &DaxIqApplet::iqRateChanged,
                     &replacementIq, &DaxIqModel::setSampleRate);
    pendingCombo->setCurrentIndex(pendingCombo->findData(96000));
    emit ThemeManager::instance().themeChanged();
    check(pendingCombo->currentData().toInt() == 96000,
          "refresh preserves a valid rate request awaiting radio confirmation");
    check(pendingCombo->accessibleDescription().contains(QStringLiteral("192"))
              && !pendingCombo->accessibleDescription().contains(QStringLiteral("not currently active")),
          "theme refresh composes per-rate reasons with confirmed active state");
    replacementIq.setSampleRate(2, 48000);
    emit ThemeManager::instance().themeChanged();
    check(pendingCombo->currentData().toInt() == 48000,
          "refresh reflects pending rate intent from a non-GUI consumer");
    replacementIq.applyStreamStatus(0x202, {{"daxiq_rate", "48000"}});
    check(!replacementIq.stream(2).rateSettling && pendingCombo->currentData().toInt() == 48000,
          "radio-confirmed rate clears pending state");
    pendingApplet.setRadioModel(nullptr);

    // Profile recovery seeds the supported remembered rate before creating.
    QSignalSpy recoveryCommands(&replacementIq, &DaxIqModel::commandReady);
    replacementIq.setSampleRate(4, replacementIq.sampleRateForPreference(192000));
    replacementIq.createStream(4);
    replacementIq.applyStreamStatus(0x204, {{"daxiq_channel", "4"}, {"daxiq_rate", "48000"}});
    bool recoveredAt96k = false;
    for (const QList<QVariant>& command : recoveryCommands) {
        recoveredAt96k = recoveredAt96k || command.first().toString().endsWith("daxiq_rate=96000");
    }
    check(recoveredAt96k, "profile recovery restores a saved 192k preference at 96k");
    return failures == 0 ? 0 : 1;
}
