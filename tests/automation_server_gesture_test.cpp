#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "models/RadioModel.h"
#include "core/AutomationServer.h"
#include "core/TxKeyingMarker.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <cstdio>
#include <functional>

using namespace AetherSDR;

namespace {

int gFailures = 0;

void expect(const char* name, bool condition)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", name);
    if (!condition) {
        ++gFailures;
    }
}

bool waitUntil(const std::function<bool()>& condition, int timeoutMs = 2000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return condition();
}

bool connectClient(QLocalSocket* socket, const QString& name)
{
    socket->connectToServer(name);
    return waitUntil([socket]() {
        return socket->state() == QLocalSocket::ConnectedState;
    });
}

QJsonObject request(QLocalSocket* socket, QJsonObject object,
                    bool authenticated = true)
{
    if (authenticated) {
        object[QStringLiteral("token")] = QStringLiteral("test-token");
    }
    socket->write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    socket->write("\n");
    socket->flush();
    if (!waitUntil([socket]() { return socket->canReadLine(); })) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), QStringLiteral("response timeout")},
        };
    }
    return QJsonDocument::fromJson(socket->readLine()).object();
}

QJsonObject gesture(QLocalSocket* socket, const QString& action,
                    const QString& target = {}, const QString& value = {},
                    bool authenticated = true)
{
    QJsonObject object{
        {QStringLiteral("cmd"), QStringLiteral("gesture")},
        {QStringLiteral("action"), action},
    };
    if (!target.isEmpty()) {
        object[QStringLiteral("target")] = target;
    }
    if (!value.isEmpty()) {
        object[QStringLiteral("value")] = value;
    }
    return request(socket, object, authenticated);
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-automation-gesture-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "FAIL could not create isolated settings profile\n");
        return 1;
    }

    qunsetenv("AETHER_AUTOMATION_ALLOW_TX");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AetherSDR-test"));
    QCoreApplication::setApplicationName(QStringLiteral("AetherSDR-test"));
    AppSettings::instance().load();

    QWidget window;
    window.resize(500, 180);
    auto* layout = new QVBoxLayout(&window);

    QSlider slider(Qt::Horizontal);
    slider.setObjectName(QStringLiteral("phaseSlider"));
    slider.setAccessibleName(QStringLiteral("Phase slider"));
    slider.setRange(0, 100);
    slider.setValue(50);
    layout->addWidget(&slider);

    QSlider powerSlider(Qt::Horizontal);
    powerSlider.setObjectName(QStringLiteral("powerSlider"));
    powerSlider.setAccessibleName(QStringLiteral("RF power"));
    powerSlider.setRange(0, 100);
    powerSlider.setValue(10);
    layout->addWidget(&powerSlider);

    QPushButton independentButton(QStringLiteral("Independent event"));
    independentButton.setObjectName(QStringLiteral("independentButton"));
    layout->addWidget(&independentButton);

    QPushButton keyingButton(QStringLiteral("PTT"));
    keyingButton.setObjectName(QStringLiteral("keyingButton"));
    markTxKeying(&keyingButton);
    layout->addWidget(&keyingButton);

    window.show();
    waitUntil([&window]() { return window.isVisible(); });

    AutomationServer server;
    server.setAuthToken(QStringLiteral("test-token"));
    qputenv("AETHER_AUTOMATION_TX_MAX_POWER", "10");
#if defined(Q_OS_UNIX)
    // Keep the AF_UNIX path below macOS's short sockaddr_un limit even when
    // the runner's TMPDIR is deeply nested.
    const QString serverName = QStringLiteral("/tmp/aether-gesture-%1")
                                   .arg(QCoreApplication::applicationPid());
#else
    const QString serverName = QStringLiteral("aether-gesture-%1")
                                   .arg(QCoreApplication::applicationPid());
#endif
    expect("server starts", server.start(serverName));
    qunsetenv("AETHER_AUTOMATION_TX_MAX_POWER");

    QLocalSocket gestureClient;
    QLocalSocket independentClient;
    expect("gesture client connects",
           connectClient(&gestureClient, server.fullServerName()));
    expect("independent client connects",
           connectClient(&independentClient, server.fullServerName()));

    const QJsonObject badBegin = gesture(
        &gestureClient, QStringLiteral("begin"), QStringLiteral("phaseSlider"),
        QStringLiteral("bad"));
    expect("malformed begin identifies local coordinates",
           !badBegin.value(QStringLiteral("ok")).toBool()
               && badBegin.value(QStringLiteral("error")).toString().contains(
                   QStringLiteral("coordinates"))
               && !badBegin.value(QStringLiteral("error")).toString().contains(
                   QStringLiteral("offsets"))
               && !slider.isSliderDown());

    const QJsonObject begun = gesture(
        &gestureClient, QStringLiteral("begin"), QStringLiteral("phaseSlider"));
    expect("begin leaves the real slider down",
           begun.value(QStringLiteral("ok")).toBool()
               && begun.value(QStringLiteral("sliderDown")).toBool()
               && slider.isSliderDown());

    bool independentClicked = false;
    QObject::connect(&independentButton, &QPushButton::clicked,
                     [&independentClicked]() { independentClicked = true; });
    const QJsonObject invoked = request(
        &independentClient,
        QJsonObject{
            {QStringLiteral("cmd"), QStringLiteral("invoke")},
            {QStringLiteral("target"), QStringLiteral("independentButton")},
            {QStringLiteral("action"), QStringLiteral("click")},
        });
    expect("independent bridge request is accepted mid-gesture",
           invoked.value(QStringLiteral("ok")).toBool());
    expect("independent event runs while slider remains down",
           waitUntil([&independentClicked]() { return independentClicked; })
               && slider.isSliderDown());

    const QJsonObject observed = gesture(
        &independentClient, QStringLiteral("status"));
    expect("second client observes active gesture without owning it",
           observed.value(QStringLiteral("active")).toBool()
               && observed.value(QStringLiteral("sliderDown")).toBool()
               && !observed.value(QStringLiteral("ownedByCaller")).toBool());

    const QJsonObject moved = gesture(
        &gestureClient, QStringLiteral("move"), {}, QStringLiteral("30 0"));
    expect("move keeps slider down",
           moved.value(QStringLiteral("ok")).toBool()
               && moved.value(QStringLiteral("sliderDown")).toBool()
               && slider.isSliderDown());
    const QJsonObject ended = gesture(
        &gestureClient, QStringLiteral("end"));
    expect("end releases slider",
           ended.value(QStringLiteral("ok")).toBool()
               && !ended.value(QStringLiteral("active")).toBool()
               && !slider.isSliderDown());

    slider.setValue(50);
    gesture(&gestureClient, QStringLiteral("begin"),
            QStringLiteral("phaseSlider"));
    const QJsonObject finalMoved = gesture(
        &gestureClient, QStringLiteral("end"), {}, QStringLiteral("60 0"));
    expect("end with an offset sends a final move before release",
           finalMoved.value(QStringLiteral("ok")).toBool()
               && finalMoved.value(QStringLiteral("dx")).toInt() == 60
               && slider.value() > 50
               && !slider.isSliderDown());

    gesture(&gestureClient, QStringLiteral("begin"),
            QStringLiteral("phaseSlider"));
    const QJsonObject badMove = gesture(
        &gestureClient, QStringLiteral("move"), {}, QStringLiteral("bad"));
    expect("malformed continuation releases slider",
           !badMove.value(QStringLiteral("ok")).toBool()
               && !slider.isSliderDown());

    gesture(&gestureClient, QStringLiteral("begin"),
            QStringLiteral("phaseSlider"));
    gestureClient.abort();
    expect("owner disconnect releases slider",
           waitUntil([&slider]() { return !slider.isSliderDown(); }));

    QLocalSocket replacementClient;
    expect("replacement client connects",
           connectClient(&replacementClient, server.fullServerName()));
    const QJsonObject unauthenticated = gesture(
        &replacementClient, QStringLiteral("begin"),
        QStringLiteral("phaseSlider"), {}, false);
    expect("authentication gate blocks gesture begin",
           !unauthenticated.value(QStringLiteral("ok")).toBool()
               && !slider.isSliderDown());

    server.setReadOnly(true);
    const QJsonObject readOnly = gesture(
        &replacementClient, QStringLiteral("begin"),
        QStringLiteral("phaseSlider"));
    expect("observe-only gate blocks gesture begin",
           !readOnly.value(QStringLiteral("ok")).toBool()
               && !slider.isSliderDown());
    server.setReadOnly(false);

    const QJsonObject keying = gesture(
        &replacementClient, QStringLiteral("begin"),
        QStringLiteral("keyingButton"));
    expect("TX-keying guard blocks phaseful gesture",
           !keying.value(QStringLiteral("ok")).toBool());
    const QJsonObject legacyDrag = request(
        &replacementClient,
        QJsonObject{
            {QStringLiteral("cmd"), QStringLiteral("drag")},
            {QStringLiteral("target"), QStringLiteral("keyingButton")},
            {QStringLiteral("value"), QStringLiteral("5 0")},
        });
    expect("TX-keying guard also covers legacy drag",
           !legacyDrag.value(QStringLiteral("ok")).toBool());

    const QJsonObject powerGesture = gesture(
        &replacementClient, QStringLiteral("begin"),
        QStringLiteral("powerSlider"));
    expect("power ceiling blocks phaseful gesture",
           !powerGesture.value(QStringLiteral("ok")).toBool()
               && !powerSlider.isSliderDown());
    const QJsonObject powerDragAt = request(
        &replacementClient,
        QJsonObject{
            {QStringLiteral("cmd"), QStringLiteral("dragAt")},
            {QStringLiteral("target"), QStringLiteral("powerSlider")},
            {QStringLiteral("value"), QStringLiteral("5 5 10 0")},
        });
    expect("power ceiling also covers concurrent dragAt integration",
           !powerDragAt.value(QStringLiteral("ok")).toBool()
               && !powerSlider.isSliderDown());

    gesture(&replacementClient, QStringLiteral("begin"),
            QStringLiteral("phaseSlider"));
    server.stop();
    expect("server stop releases slider", !slider.isSliderDown());

    if (gFailures == 0) {
        std::printf("\nAll phaseful-gesture checks passed.\n");
    } else {
        std::printf("\n%d phaseful-gesture checks failed.\n", gFailures);
    }
    return gFailures == 0 ? 0 : 1;
}
