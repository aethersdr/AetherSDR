// Socket-free: real backend/session objects, but no session.start(), sockets,
// peer firmware, capture device, or live radio. Feed literal CI-V replies.
#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/icom/IcomSession.h"
#include "core/AudioEngine.h"
#include "models/RadioModel.h"
#include "core/backends/icom/IcomSettings.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>
#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::icom;

namespace AetherSDR::icom {
struct IcomCivBackendTestAccess {
    static void connect(IcomCivBackend& b, const QString& name,
                        std::uint8_t seed = 0xA4, bool pinned = false)
    {
        b.disconnectRadio();
        b.m_civTrace.clear();
        b.m_session = std::make_unique<IcomSession>();
        b.m_session->setCivAddress(seed);
        b.m_civSeedAddress = seed;
        b.m_civAddressPinned = pinned;
        b.onSessionConnected(name);
    }
    static void inject(IcomCivBackend& b, const QByteArray& bytes,
                       std::uint64_t generation)
    {
        const auto frame = parseFrame({
            reinterpret_cast<const std::uint8_t*>(bytes.constData()),
            static_cast<std::size_t>(bytes.size())});
        if (frame) {
            b.onCivFrame(*frame, generation);
        }
    }
    static void inject(IcomCivBackend& b, const char* hex)
    {
        inject(b, QByteArray::fromHex(hex), b.m_sessionGeneration);
    }
    static std::uint8_t address(const IcomCivBackend& b) { return b.m_session->civAddress(); }
    static std::uint64_t generation(const IcomCivBackend& b) { return b.m_sessionGeneration; }
    static bool ambiguous(const IcomCivBackend& b) { return b.m_civAmbiguous; }
    static void markKeyed(IcomCivBackend& b) { b.m_keyed = true; }
    static bool keyed(const IcomCivBackend& b) { return b.m_keyed; }
    static void enableWake(IcomCivBackend& b, uint modelId)
    { b.m_wakeOnConnect = true; b.m_wakeModelId = modelId; }
    static void timeout(IcomCivBackend& b) { b.requestCivIdentity(b.m_sessionGeneration); }
    static void staleRetry(IcomCivBackend& b, std::uint64_t generation)
    { b.requestCivIdentity(generation); }
    static int attempts(const IcomCivBackend& b) { return b.m_civDetectAttempts; }
    static bool retrying(const IcomCivBackend& b)
    { return b.m_civDetectTimer && b.m_civDetectTimer->isActive(); }
    static QStringList outbound(const IcomCivBackend& b)
    {
        QStringList frames;
        for (const auto& e : b.m_civTrace) {
            if (e.outbound) { frames << e.hex; }
        }
        return frames;
    }
};
}

namespace AetherSDR {
struct RadioModelWakeTestAccess {
    static IcomCivBackend& prepare(RadioModel& model)
    {
        model.teardownBackend();
        model.setupBackend(QStringLiteral("icom"));
        model.m_lastInfo.address = QHostAddress(QStringLiteral("127.0.0.1"));
        model.m_lastInfo.family = QStringLiteral("icom");
        return *static_cast<IcomCivBackend*>(model.m_backend.get());
    }
    static bool retrying(const RadioModel& model) { return model.m_reconnectTimer.isActive(); }
    static quint64 generation(const RadioModel& model) { return model.m_radioWakeGeneration; }
    static void fail(RadioModel& model) { model.onConnectionError(QStringLiteral("injected wake failure")); }
};
}

namespace {
int failures = 0;
void waitMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
void check(bool ok, const char* message)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir settings;
    qputenv("AETHER_SETTINGS_DIR", settings.path().toUtf8());
    IcomSettings::reset();
    check(!IcomSettings::wakeOnConnect(), "wake-on-connect defaults off");
    IcomSettings::setWakeOnConnect(true);
    check(IcomSettings::wakeOnConnect(), "wake policy round-trips through the Icom document");
    IcomSettings::setWakeOnConnect(false);
    AudioEngine audio;
    IcomCivBackend backend;
    QString nickname;
    QStringList antennas;
    QStringList preamps;
    int publications = 0;
    int audioFrames = 0;
    QObject::connect(&backend, &IRadioBackend::radioChanged,
                     [&](const RadioDelta& d) {
        if (d.nickname) { nickname = *d.nickname; }
    });
    QObject::connect(&backend, &IRadioBackend::sliceChanged,
                     [&](int, const SliceDelta& d) {
        if (d.rxAntennaList) { antennas = *d.rxAntennaList; }
    });
    QObject::connect(&backend, &IRadioBackend::panPreampInfoChanged,
                     [&](const QString&, const QStringList& labels) { preamps = labels; });
    QObject::connect(&backend, &IRadioBackend::capabilitiesChanged, [&] {
        ++publications;
        // Exercise the production lifecycle with capture off: TCI PCM does not
        // require a microphone. No QAudioSource or network TX stream is opened.
        audio.applyBackendAudioCapabilities(backend.isConnected(), backend.capabilities(),
                                            false, {});
    });
    QObject::connect(&audio, &AudioEngine::txFinalMonitorPcmReady,
                     [&](const QByteArray& pcm, bool clientLeveled) {
        check(pcm.size() == 1920 && clientLeveled, "TCI stereo PCM reaches the backend seam intact");
        ++audioFrames;
        // The real Icom submission gate must drop this while unkeyed.
        backend.submitTxAudio(pcm, 24000, clientLeveled);
    });
    const QByteArray pcm(960 * sizeof(float), '\0');
    for (const QString& name : {QStringLiteral("Shack portable"), QStringLiteral("IC-7300MK2"),
                                QStringLiteral("IC-705"), QString{}}) {
        IcomCivBackendTestAccess::connect(backend, name);
        check(!backend.capabilities().canTransmit && !audio.hostModulation(),
              "every name starts unidentified with TX audio disabled");
        check(nickname == (name.isEmpty() ? QStringLiteral("Unknown Icom") : name),
              "the network name remains presentation text");
        const int before = audioFrames;
        audio.feedDaxTxAudio(pcm);
        check(audioFrames == before, "unidentified backend cannot receive TCI PCM");
        // IC-705 model ID A4 at customized bus address 94 (IC-7300's default).
        IcomCivBackendTestAccess::inject(backend, "fefee0941900a4fd");
        check(backend.capabilities().model == QStringLiteral("IC-705")
                  && backend.capabilities().canTransmit && audio.hostModulation(),
              "payload A4 selects IC-705 regardless of nickname or address 94");
        check(IcomCivBackendTestAccess::address(backend) == 0x94,
              "commands use the envelope source, never the model ID");
        check(nickname == (name.isEmpty() ? QStringLiteral("IC-705") : name),
              "identification preserves the nickname or supplies the empty-name fallback");
        check(!preamps.isEmpty(), "late identity publishes the front-end controls");
        audio.feedDaxTxAudio(pcm);
        check(audioFrames == before + 1, "late identification enables actual TCI PCM delivery");
        const int published = publications;
        IcomCivBackendTestAccess::inject(backend, "fefee0941900a4fd");
        check(publications == published, "duplicate ID confirmation is inert");
        check(!audio.isTxStreaming(), "TCI-only operation does not open microphone capture");
    }
    IcomCivBackendTestAccess::connect(backend, "Desktop");
    IcomCivBackendTestAccess::timeout(backend);
    IcomCivBackendTestAccess::inject(backend, "fefee0501900b6fd");
    check(backend.capabilities().model == QStringLiteral("IC-7300MK2")
              && IcomCivBackendTestAccess::address(backend) == 0x50,
          "IC-7300MK2 resolves after timeout at a customized address");
    check(antennas.contains(QStringLiteral("RX-ANT")),
          "late IC-7300MK2 identity publishes its antenna choices");

    IcomCivBackendTestAccess::connect(backend, "IC-705", 0x50, true);
    IcomCivBackendTestAccess::inject(backend, "fefee0a41900a4fd");
    check(!backend.capabilities().canTransmit, "pinned selection ignores another responder");
    IcomCivBackendTestAccess::inject(backend, "fefee0501900b6fd");
    check(backend.capabilities().model == QStringLiteral("IC-7300MK2"),
          "pinned address identifies from its own payload");

    IcomCivBackendTestAccess::connect(backend, "IC-705");
    for (const char* bad : {"fefee0a41900fd", "fefee0a41900a400fd", "fefee0a41901a4fd",
                            "fefe00e01900a4fd", "fefee0001900a4fd", "fefee1a41900a4fd"}) {
        IcomCivBackendTestAccess::inject(backend, bad);
        check(!backend.capabilities().canTransmit, "malformed or foreign ID cannot enable TX");
    }
    IcomCivBackendTestAccess::inject(backend, "fefee0a419007ffd");
    check(!backend.capabilities().canTransmit && !audio.hostModulation(),
          "unknown ID cannot inherit the familiar nickname or source-address profile");

    IcomCivBackendTestAccess::connect(backend, "IC-705");
    IcomCivBackendTestAccess::inject(backend, "fefee0501900a4fd");
    IcomCivBackendTestAccess::markKeyed(backend);
    IcomCivBackendTestAccess::inject(backend, "fefee0511900a4fd");
    check(!IcomCivBackendTestAccess::keyed(backend)
              && IcomCivBackendTestAccess::address(backend) == 0x50,
          "ambiguity releases the previously selected destination without retargeting unkey");
    check(IcomCivBackendTestAccess::ambiguous(backend)
              && !backend.capabilities().canTransmit && !audio.hostModulation(),
          "two responders with the SAME model ID revoke identity and the audio route");
    const int before = audioFrames;
    audio.feedDaxTxAudio(pcm);
    check(audioFrames == before, "capability withdrawal stops TCI PCM delivery");
    IcomCivBackendTestAccess::inject(backend, "fefee0501900a4fd");
    check(!backend.capabilities().canTransmit, "late duplicate cannot undo ambiguous-bus rejection");
    const auto oldGeneration = IcomCivBackendTestAccess::generation(backend);
    IcomCivBackendTestAccess::connect(backend, "Renamed desktop");
    IcomCivBackendTestAccess::inject(backend, QByteArray::fromHex("fefee0501900a4fd"), oldGeneration);
    check(!backend.capabilities().canTransmit, "old-session identity cannot leak across reconnect");
    IcomCivBackendTestAccess::inject(backend, "fefee0b61900b6fd");
    check(backend.capabilities().model == QStringLiteral("IC-7300MK2"),
          "reconnect clears ambiguity and resolves the newly selected radio");
    audio.applyBackendAudioCapabilities(false, backend.capabilities(), false, {});
    check(!audio.hostModulation(), "disconnect clears the seam route");
    for (const QString& family : {QStringLiteral("flex"), QStringLiteral("sim"),
                                  QStringLiteral("anan"), QStringLiteral("rtl")}) {
        RadioCapabilities caps;
        caps.family = family;
        caps.takesTxAudioOverSeam = false;
        audio.applyBackendAudioCapabilities(true, caps, false, {});
        check(!audio.hostModulation() && !audio.isTxStreaming(),
              "non-seam families do not acquire seam capture");
    }
    RadioCapabilities rxOnly;
    rxOnly.takesTxAudioOverSeam = true;
    rxOnly.canTransmit = false;
    audio.applyBackendAudioCapabilities(true, rxOnly, false, {});
    check(!audio.hostModulation(), "RX-only seam backend remains disabled");
    // Real timer/scheduler lifecycle with a silent injected transport. The
    // initial broadcast may time out or receive only FB/FA during pipe startup.
    const QString broadcast = QStringLiteral("fe fe 00 e0 19 00 fd");
    for (const char* earlyReply : {"", "fefee0b6fbfd", "fefee0b6fafd"}) {
        IcomCivBackendTestAccess::connect(backend, "Renamed radio");
        if (*earlyReply) {
            IcomCivBackendTestAccess::inject(backend, earlyReply);
        }
        waitMs(1250);
        check(IcomCivBackendTestAccess::outbound(backend)
                  == QStringList{broadcast, broadcast},
              "missed identity or generic ACK retries broadcast without polling seed A4");
        check(!backend.capabilities().canTransmit && !audio.hostModulation(),
              "a generic ACK cannot finish model identification");
        IcomCivBackendTestAccess::inject(backend, "fefee0b61900b6fd");
        check(backend.capabilities().model == QStringLiteral("IC-7300MK2")
                  && IcomCivBackendTestAccess::address(backend) == 0xB6
                  && audio.hostModulation(),
              "a later B6 reply completes startup and enables the audio route");
        check(!IcomCivBackendTestAccess::retrying(backend),
              "valid model reply stops identification retries");
    }
    IcomCivBackendTestAccess::connect(backend, "Pinned radio", 0x50, true);
    waitMs(1250);
    check(IcomCivBackendTestAccess::outbound(backend)
              == QStringList{QStringLiteral("fe fe 50 e0 19 00 fd"),
                             QStringLiteral("fe fe 50 e0 19 00 fd")},
          "a pinned selection retries only its selected destination");
    const auto previousGeneration = IcomCivBackendTestAccess::generation(backend);
    IcomCivBackendTestAccess::connect(backend, "New session");
    IcomCivBackendTestAccess::staleRetry(backend, previousGeneration);
    check(IcomCivBackendTestAccess::attempts(backend) == 1,
          "queued retry from an old session cannot advance new discovery");
    int warnings = 0;
    QObject::connect(&backend, &IRadioBackend::configurationWarning,
                     [&](const QString&) { ++warnings; });
    waitMs(6500);
    check(IcomCivBackendTestAccess::outbound(backend)
              == QStringList{broadcast, broadcast, broadcast, broadcast, broadcast},
          "silent startup sends exactly five identity queries and no seed-address reads");
    check(warnings == 1 && !IcomCivBackendTestAccess::retrying(backend)
              && !backend.capabilities().canTransmit,
          "retry exhaustion reports one actionable error and stays conservative");
    backend.disconnectRadio();
    check(!IcomCivBackendTestAccess::retrying(backend),
          "disconnect cancels pending discovery");
    backend.disconnectRadio();
    int wakeRequests = 0;
    QObject::connect(&backend, &IRadioBackend::extensionStatus,
        [&](const QString& ns, const QString& kind, const QVariantMap&) {
            if (ns == "icom" && kind == "power.wakeNeeded") { ++wakeRequests; }
        });
    IcomCivBackendTestAccess::connect(backend, "IC-9700");
    for (int i = 0; i < 5; ++i) { IcomCivBackendTestAccess::timeout(backend); }
    check(wakeRequests == 0, "ordinary connect never requests power despite its nickname");
    IcomCivBackendTestAccess::connect(backend, "Renamed radio", 0xA2);
    IcomCivBackendTestAccess::enableWake(backend, 0xA2);
    for (int i = 0; i < 5; ++i) { IcomCivBackendTestAccess::timeout(backend); }
    check(wakeRequests == 1, "opt-in plus explicit model requests wake after bounded discovery");
    IcomCivBackendTestAccess::timeout(backend);
    check(wakeRequests == 1, "the same failed discovery cannot request a second wake");
    backend.disconnectRadio();
    // The real model and backend, with an UNSTARTED session: neither wake
    // nor teardown opens a socket. Cancel every reconnect before its delay.
    {
        RadioModel radio;
        IcomCivBackend& wakeBackend = RadioModelWakeTestAccess::prepare(radio);
        IcomCivBackendTestAccess::connect(wakeBackend, "IC-9700");
        QString error;
        check(!radio.wakeIcomRadio(0xA4, 0xA4, &error)
                  && !radio.wakeIcomRadio(0xB6, 0xB6, &error),
              "IC-705 and MK2 do not inherit the IC-9700 network wake profile");
        check(!radio.wakeIcomRadio(0xA2, 0, &error)
                  && !radio.wakeIcomRadio(0xA2, 0xE0, &error)
                  && !radio.wakeIcomRadio(0x1A2, 0xA2, &error),
              "broadcast, controller and out-of-range model selections refuse wake");
        check(radio.wakeIcomRadio(0xA2, 0x50, &error),
              "explicit IC-9700 wake accepts an independently chosen address");
        QStringList frames = IcomCivBackendTestAccess::outbound(wakeBackend);
        int wakeWrites = 0;
        for (const QString& frame : frames) {
            if (frame.endsWith(QStringLiteral("50 e1 18 01 fd"))) { ++wakeWrites; }
        }
        check(wakeWrites == 1 && radio.radioWakeActive()
                  && !RadioModelWakeTestAccess::retrying(radio),
              "one explicit wake has no repeating reconnect timer");
        check(!radio.wakeIcomRadio(0xA2, 0x50, &error),
              "an active wake cannot send another power command");
        const quint64 wakeGeneration = RadioModelWakeTestAccess::generation(radio);
        radio.disconnectFromRadio();
        check(!radio.radioWakeActive()
                  && RadioModelWakeTestAccess::generation(radio) != wakeGeneration
                  && !RadioModelWakeTestAccess::retrying(radio),
              "operator disconnect invalidates delayed wake callbacks");
        IcomCivBackendTestAccess::connect(wakeBackend, "IC-9700");
        IcomCivBackendTestAccess::inject(wakeBackend, "fefee0b61900b6fd");
        check(!radio.wakeIcomRadio(0xA2, 0xB6, &error),
              "a misleading nickname cannot override wire identity for wake");
        IcomCivBackendTestAccess::connect(wakeBackend, "Custom name", 0x50, true);
        check(!radio.wakeIcomRadio(0xA2, 0xA2, &error),
              "wake cannot redirect a pinned session");
        IcomCivBackendTestAccess::markKeyed(wakeBackend);
        check(!radio.wakeIcomRadio(0xA2, 0x50, &error),
              "wake refuses an active transmitter");
        IcomCivBackendTestAccess::inject(wakeBackend, "fefee0501c0000fd");
        IcomCivBackendTestAccess::connect(wakeBackend, "Custom name");
        check(radio.wakeIcomRadio(0xA2, 0xA2, &error), "second explicit operation starts");
        RadioModelWakeTestAccess::fail(radio);
        waitMs(20);
        check(!radio.radioWakeActive() && !radio.isConnected()
                  && !RadioModelWakeTestAccess::retrying(radio),
              "terminal wake failure disconnects without scheduling another attempt");
        IcomCivBackendTestAccess::connect(wakeBackend, "Custom name");
        check(radio.wakeIcomRadio(0xA2, 0xA2, &error), "readiness operation starts");
        // Inject the post-wake session and its identity, without running start().
        IcomCivBackendTestAccess::connect(wakeBackend, "Renamed IC-9700", 0xA2, true);
        IcomCivBackendTestAccess::inject(wakeBackend, "fefee0a21900a2fd");
        check(!radio.radioWakeActive() && radio.isConnected(),
              "matching wire identity completes wake and invalidates its deadline");
        radio.disconnectFromRadio();
    }
    std::printf("icom_identity_test: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
