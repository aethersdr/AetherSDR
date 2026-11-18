// Production session lifetime and our HTTP transport are injected in memory.
// No socket, simulated radio server, discovery or RF command is used.
#include "AetherPDTestRadio.h"
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/txlinearity/AetherPDCaptureSource.h"
#include "core/txlinearity/AetherPDTestClient.h"


int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aetherpd_session_test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    using namespace AetherSDR;
    using namespace AetherSDR::aetherpd;
    using Access = SessionControlTestAccess;
    AppSettings::instance().load();
    Network network;
    Network captureNetwork;
    txlin::AetherPDCaptureSource capture(captureNetwork);
    Network testNetwork;
    txlin::AetherPDTestClient helper(testNetwork, [] { return 1000; });
    RadioModel radio;
    const QString token(43, u'a');
    bool ok = true;
    ok &= check(!radio.attachAetherPD(radio.controlSession(), token)
        && network.sent.empty(), "disconnected radio refuses attachment");
    Access::install(radio, network);
    if (!check(radio.automationApplySliceFixture(0, QStringLiteral("A")),
        "injected active USB receiver can be created while disconnected")) {
        return 1;
    }
    Access::begin(radio);
    RadioControlSession session = radio.controlSession();
    ok &= check(radio.attachAetherPD(session, token) && network.sent.size() == 1,
        "registered current GUI can attach to current radio endpoint");
    QFile file(QStringLiteral(AETHERPD_TESTDATA_DIR "/idle.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return 1;
    }
    const QByteArray idle = file.readAll();
    network.last->complete(200, idle);
    ok &= check(radio.aetherPDModel().status().has_value(), "owned model publishes observed status");
    const std::function<bool()> initialCapture = radio.bindAetherPDCapture(session, capture);
    ok &= check(initialCapture && initialCapture() && captureNetwork.sent.empty(),
                "actual registered RadioModel could not bind receive-only capture");
    const std::function<bool()> initialTest = radio.bindAetherPDTest(session, helper);
    ok &= check(initialTest && initialTest() && testNetwork.sent.empty(), "registered model could not bind unused helper");
    ok &= check(radio.requestAetherPD(session, Operation::ArmCalibration), "current session admits explicit arm");
    ok &= check(!initialCapture(), "control change retained old measurement preparation");
    ok &= check(!initialTest(), "control change retained helper binding");
    Reply* oldArm = network.last;
    bool reattachedDuringRetirement = false;
    const QMetaObject::Connection attempt = QObject::connect(&radio.aetherPDModel(), &AetherPDModel::changed,
        &app, [&] {
            if (!radio.aetherPDModel().active()) {
                reattachedDuringRetirement = radio.attachAetherPD(session, token);
            }
        });
    const size_t beforeRetire = network.sent.size();
    Access::retire(radio);
    QObject::disconnect(attempt);
    oldArm->complete(200, R"({"state":"calibration_arm_accepted"})");
    ok &= check(oldArm->aborted && !radio.aetherPDModel().active()
        && !radio.aetherPDModel().status() && !reattachedDuringRetirement
        && network.sent.size() == beforeRetire,
        "session guard retires before PD notifications; late arm cannot revive it");

    Access::begin(radio);
    ok &= check(!radio.attachAetherPD(session, token)
        && !radio.requestAetherPD(session, Operation::Stop) && network.sent.size() == beforeRetire,
        "old dialog or credential callback cannot act on same-address reconnect");
    const RadioControlSession oldSession = session;
    session = radio.controlSession();
    ok &= check(radio.attachAetherPD(session, token), "new connection requires explicit attachment");
    ok &= check(!radio.detachAetherPD(oldSession) && radio.aetherPDModel().active(),
        "old dialog teardown cannot detach newer connection");
    network.last->complete(200, idle);
    ok &= check(!radio.bindAetherPDCapture(oldSession, capture), "retired dialog bound capture to a new connection");
    const std::function<bool()> guiCapture = radio.bindAetherPDCapture(session, capture);
    ok &= check(guiCapture && guiCapture(), "replacement RadioModel session could not bind capture");
    ok &= check(!radio.bindAetherPDCapture(oldSession, capture) && guiCapture(),
                "retired panel changed a newer capture binding");
    const std::function<bool()> guiTest = radio.bindAetherPDTest(session, helper);
    ok &= check(guiTest && guiTest() && !radio.bindAetherPDTest(oldSession, helper)
        && guiTest() && helper.begin(15000) && testNetwork.sent.size() == 1,
        "retired panel changed replacement helper binding");
    Access::changeGui(radio);
    ok &= check(!guiTest(), "GUI identity replacement retained helper binding");
    testNetwork.last->complete(200, R"({"test_id":"0123456789abcdef0123456789abcdef","state":"observing","cleanup_due_ms":"20000","remaining_ms":15000})");
    ok &= check(helper.remainingMs() == 0 && testNetwork.sent.size() == 1,
        "late helper response advanced a retired GUI session");
    ok &= check(!guiCapture(), "old GUI identity retained measurement authority");
    const size_t beforeGuiRequest = network.sent.size();
    ok &= check(!radio.aetherPDModel().active() && !radio.aetherPDModel().status()
        && !radio.requestAetherPD(session, Operation::ArmCalibration)
        && network.sent.size() == beforeGuiRequest, "GUI identity replacement retires PD without RF or rearm");

    Access::begin(radio);
    session = radio.controlSession();
    radio.attachAetherPD(session, token);
    network.last->complete(200, idle);
    const std::function<bool()> transportCapture = radio.bindAetherPDCapture(session, capture);
    ok &= check(transportCapture && transportCapture(), "capture could not bind before transport test");
    radio.requestAetherPD(session, Operation::Status);
    Reply* oldStatus = network.last;
    Access::changeTransport(radio);
    ok &= check(!transportCapture(), "replaced transport retained measurement authority");
    const size_t beforeTransportRequest = network.sent.size();
    ok &= check(!radio.aetherPDModel().active()
        && !radio.requestAetherPD(session, Operation::Stop)
        && network.sent.size() == beforeTransportRequest, "wire replacement fences requests before disconnect notification");
    oldStatus->complete(200, idle);
    ok &= check(!radio.aetherPDModel().status() && !radio.aetherPDModel().pendingOperation(),
        "reply from replaced transport cannot publish state");

    Access::begin(radio);
    radio.attachAetherPD(radio.controlSession(), token);
    oldStatus = network.last;
    radio.disconnectFromRadio();
    oldStatus->complete(200, idle);
    ok &= check(oldStatus->aborted && !radio.aetherPDModel().active()
        && !radio.aetherPDModel().status(), "public disconnect clears PD before transport teardown");
    const size_t afterDisconnect = network.sent.size();
    ok &= check(!radio.attachAetherPD(radio.controlSession(), token)
        && !radio.requestAetherPD(radio.controlSession(), Operation::Start)
        && network.sent.size() == afterDisconnect, "closing session refuses startup before reconnect");
    Access::begin(radio, 8);
    session = radio.controlSession();
    ok &= check(radio.attachAetherPD(session, token)
        && radio.requestAetherPD(session, Operation::Start), "current registered session can explicitly start");
    ok &= check(network.sent.back().body == *startBody({QStringLiteral("0D3EA0E8-B052-4AC6-9D32-D96918634469"), 8}),
                "production RadioModel supplies observed GUI identity, not a persisted handle");
    Reply* oldStart = network.last;
    Access::retire(radio);
    oldStart->complete(202, R"({"state":"start_requested"})");
    ok &= check(oldStart->aborted && !radio.aetherPDModel().lastAcknowledgment()
        && !radio.requestAetherPD(session, Operation::Start),
        "retiring the production session fences startup and its late acknowledgment");
    std::cout << (ok ? "PASS" : "FAIL") << ": actual RadioModel PD lifetime and GUI/transport fences\n";
    return ok ? 0 : 1;
}
