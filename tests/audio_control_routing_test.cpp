// Production encoders and model admission, with unopened transports only.
#include "TestSettingsProfile.h"
#include "PanControlTestFixture.h"
#include "core/backends/sim/SimBackend.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <cstdio>
#include <thread>
#include <vector>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool passed, const char* message)
{
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
    failures += !passed;
}
void events()
{
    QEventLoop loop;
    QTimer::singleShot(0, &loop, &QEventLoop::quit);
    loop.exec();
}
struct AudioFixture : PanControlFixture {
    std::vector<AudioControlGuard> guards;
    AudioFixture()
    {
        backend->setAudioCommandSink([this](const QString& command, FlexBackend::CommandReply reply,
                                           const AudioControlGuard& guard) {
            if (!backend->admit || !guard.permits()) { return false; }
            guards.push_back(guard);
            backend->writes.push_back({command, [reply](const RadioControlReply& result) {
                if (reply) { reply(result.code, result.message); }
            }});
            return true;
        });
    }
    void stream(quint32 id, const QString& type = QStringLiteral("dax_iq"),
                const QString& owner = QStringLiteral("0x71"), int channel = 1)
    {
        QMap<QString, QString> fields{{"type", type}, {"client_handle", owner}};
        if (type == QLatin1String("dax_iq")) { fields.insert("daxiq_channel", QString::number(channel)); }
        if (type == QLatin1String("dax_rx")) { fields.insert("dax_channel", QString::number(channel)); }
        status(QStringLiteral("stream 0x%1").arg(id, 0, 16), fields);
    }
    void answer(size_t index, quint32 id)
    { backend->answer(index, {RadioControlReply::Kind::Accepted, 0, QStringLiteral("0x%1").arg(id, 0, 16)}); }
};

void encoding()
{
    AudioFixture f;
    struct Case { AudioStreamRequest request; const char* expected; };
    const Case cases[]{
        {{AudioStreamKind::RemoteReceive}, "stream create type=remote_audio_rx compression=none"},
        {{AudioStreamKind::RemoteTransmit}, "stream create type=remote_audio_tx compression=opus"},
        {{AudioStreamKind::DaxReceive, AudioStreamAction::Create, 0, 8}, "stream create type=dax_rx dax_channel=8"},
        {{AudioStreamKind::DaxTransmit}, "stream create type=dax_tx"},
        {{AudioStreamKind::Iq, AudioStreamAction::Create, 0, 4}, "stream create type=dax_iq daxiq_channel=4"},
        {{AudioStreamKind::Iq, AudioStreamAction::Remove, 0x123, 4}, "stream remove 0x123"},
        {{AudioStreamKind::Iq, AudioStreamAction::SetIqRate, 0x123, 4, 192000}, "stream set 0x123 daxiq_rate=192000"},
        {{AudioStreamKind::RemoteReceive, AudioStreamAction::Create, 0, 0, 0, AudioCompression::Opus},
            "stream create type=remote_audio_rx compression=opus"}
    };
    for (const Case& entry : cases) {
        AudioStreamRequest request = entry.request;
        request.guard = AudioControlGuard::create(30000);
        check(f.backend->requestAudioStream(request) == ReceiveDispatch::Dispatched
            && f.backend->commands().last() == QString::fromLatin1(entry.expected), entry.expected);
    }
    const size_t before = f.backend->writes.size();
    const AudioStreamRequest invalid[]{
        {AudioStreamKind::Iq, AudioStreamAction::Create, 0, 0},
        {AudioStreamKind::Iq, AudioStreamAction::Create, 0, 5},
        {AudioStreamKind::DaxReceive, AudioStreamAction::Create, 0, 9},
        {AudioStreamKind::Iq, AudioStreamAction::Remove, 0, 1},
        {AudioStreamKind::Iq, AudioStreamAction::SetIqRate, 0x123, 1, 44100},
        {static_cast<AudioStreamKind>(99)},
        {AudioStreamKind::Iq, static_cast<AudioStreamAction>(99), 0, 1}
    };
    for (AudioStreamRequest request : invalid) {
        request.guard = AudioControlGuard::create(30000);
        check(f.radio.requestAudioStream(request) == ReceiveDispatch::Unsupported, "invalid typed stream request refused");
    }
    check(f.backend->writes.size() == before, "invalid requests never reach encoder sink");
    for (int band = 0; band < 8; ++band) {
        check(f.radio.requestGraphicEq({GraphicEqRequest::Path::Receive, GraphicEqRequest::Field::BandGain, band, band - 4})
            == ReceiveDispatch::Dispatched, "each graphic EQ band dispatched");
    }
    check(f.backend->commands().last() == QStringLiteral("eq RXsc 8000Hz=3"), "EQ uses the canonical band spelling");
    f.radio.requestGraphicEq({GraphicEqRequest::Path::Transmit, GraphicEqRequest::Field::Enabled, 0, 1});
    check(f.backend->commands().last() == QStringLiteral("eq TXsc mode=True"), "EQ enable uses FlexLib boolean spelling");
    check(f.radio.requestGraphicEq({GraphicEqRequest::Path::Receive, GraphicEqRequest::Field::BandGain, 8, 1})
        == ReceiveDispatch::Unsupported, "invalid EQ band refused before indexing");
    f.backend->setAudioCommandSink([](const QString&, FlexBackend::CommandReply reply, const AudioControlGuard&) {
        reply(0, QStringLiteral("0x100"));
        return false;
    });
    int calls = 0;
    AudioStreamRequest refused{AudioStreamKind::RemoteReceive};
    refused.guard = AudioControlGuard::create(30000);
    check(f.backend->requestAudioStream(refused, [&](const AudioStreamReply& reply) {
        ++calls;
        check(reply.result.kind == RadioControlReply::Kind::Failed && reply.streamId == 0,
            "a synchronous reply cannot override terminal dispatch refusal");
    }) == ReceiveDispatch::Unsupported && calls == 1, "dispatch refusal completes exactly once");
}

void identityAndReplies()
{
    for (bool statusFirst : {false, true}) {
        AudioFixture f;
        int calls = 0;
        AudioStreamReply result;
        f.radio.requestAudioStream({AudioStreamKind::Iq, AudioStreamAction::Create, 0, 1},
            [&](const AudioStreamReply& reply) { ++calls; result = reply; });
        if (statusFirst) { f.stream(0x100); }
        f.answer(0, 0x100);
        if (!statusFirst) { f.stream(0x100); }
        f.answer(0, 0x200);
        check(calls == 1 && result.result.kind == RadioControlReply::Kind::Accepted && result.streamId == 0x100,
            "ACK/status in either order completes once with the allocated identity");
        check(f.radio.requestAudioStream({AudioStreamKind::Iq, AudioStreamAction::SetIqRate, 0x100, 1, 96000})
            == ReceiveDispatch::Dispatched, "owned stream accepts a rate request");
        const AudioControlGuard old = f.guards.back();
        f.stream(0x100, QStringLiteral("dax_iq"), QStringLiteral("0x72"));
        f.stream(0x100);
        check(!old.permits(), "A/B/A ownership never revives an already queued mutation");
    }
    for (const QString& owner : {QStringLiteral("0x72"), QStringLiteral("broken"), QStringLiteral("0x0")}) {
        AudioFixture f;
        f.stream(0x100, QStringLiteral("dax_iq"), owner);
        check(f.radio.requestAudioStream({AudioStreamKind::Iq, AudioStreamAction::Remove, 0x100, 1})
            == ReceiveDispatch::Unsupported && f.backend->writes.empty(), "foreign or malformed ownership cannot remove a stream");
    }
    {
        AudioFixture f;
        f.stream(0x100);
        f.radio.requestAudioStream({AudioStreamKind::Iq, AudioStreamAction::SetIqRate, 0x100, 1, 96000});
        const AudioControlGuard first = f.guards.back();
        f.radio.requestAudioStream({AudioStreamKind::Iq, AudioStreamAction::SetIqRate, 0x100, 1, 48000});
        check(!first.permits() && f.guards.back().permits(), "latest IQ rate intent revokes the previous queued write");
        const AudioControlGuard second = f.guards.back();
        f.status(QStringLiteral("stream 0x100"), {{"type", "other_stream"}});
        check(!second.permits(), "a changed stream type retires the old identity");
    }
}

void cancellation()
{
    {
        AudioFixture f;
        AudioStreamRequest request{AudioStreamKind::Iq, AudioStreamAction::Create, 0, 1};
        request.guard = AudioControlGuard::create(30000);
        int calls = 0;
        f.radio.requestAudioStream(request, [&](const AudioStreamReply& reply) {
            ++calls;
            check(reply.result.kind == RadioControlReply::Kind::Cancelled, "revoked allocation reports cancellation");
        });
        request.guard.revoke();
        f.answer(0, 0x100);
        f.answer(0, 0x100);
        check(calls == 1 && f.backend->commands().count(QStringLiteral("stream remove 0x100")) == 1,
            "late canceled create is cleaned once in its original session");
    }
    for (int scenario = 0; scenario < 4; ++scenario) {
        AudioFixture f;
        if (scenario == 3) { f.stream(0x100); }
        AudioStreamRequest request{AudioStreamKind::Iq, AudioStreamAction::Create, 0, 1};
        request.guard = AudioControlGuard::create(30000);
        f.radio.requestAudioStream(request);
        request.guard.revoke();
        if (scenario == 0) { TxOperationIntegrationTestAccess::newSession(f.radio); }
        if (scenario == 1) { f.stream(0x100, QStringLiteral("dax_iq"), QStringLiteral("0x72")); }
        if (scenario == 2) { f.status(QStringLiteral("stream 0x100 removed"), {}); f.stream(0x100); }
        f.answer(0, 0x100);
        check(f.backend->writes.size() == 1, "late reply cannot clean a new session, foreign, reused or preexisting ID");
    }
    {
        AudioFixture f;
        int calls = 0;
        f.radio.requestAudioStream({AudioStreamKind::Iq, AudioStreamAction::Create, 0, 1},
            [&](const AudioStreamReply&) { ++calls; });
        QTimer* deadline = f.radio.findChild<QTimer*>(QStringLiteral("audioControlDeadline"));
        check(deadline && QMetaObject::invokeMethod(deadline, "timeout", Qt::DirectConnection), "timeout driven without a sleep");
        check(calls == 1 && !f.guards.front().permits() && TxOperationIntegrationTestAccess::pendingAudio(f.radio) == 1,
            "timeout closes admission but retains the unknown allocation reservation");
        check(f.radio.requestAudioStream({AudioStreamKind::Iq, AudioStreamAction::Create, 0, 1}) == ReceiveDispatch::Unsupported,
            "unknown allocation is not retried");
        f.answer(0, 0x100);
        check(calls == 1 && f.backend->commands().last() == QStringLiteral("stream remove 0x100"),
            "late timeout reply is cleaned without a second completion");
    }
    {
        AudioFixture f;
        f.radio.requestAudioStream({AudioStreamKind::Iq, AudioStreamAction::Create, 0, 1});
        f.backend->answer(0, {RadioControlReply::Kind::Accepted, 0, QStringLiteral("garbage")});
        check(TxOperationIntegrationTestAccess::pendingAudio(f.radio) == 1
            && f.radio.requestAudioStream({AudioStreamKind::Iq, AudioStreamAction::Create, 0, 1}) == ReceiveDispatch::Unsupported,
            "malformed successful allocation also retains a bounded reservation");
    }
}

void iqAndBinding()
{
    AudioFixture f;
    DaxIqModel& iq = f.radio.daxIqModel();
    check(iq.createStream(1) && iq.createPending(1), "IQ On starts one allocation");
    iq.removeStream(1);
    check(!f.guards.front().permits(), "IQ Off revokes the unanswered create at the terminal writer");
    iq.createStream(1);
    f.answer(0, 0x100);
    iq.applyStreamStatus(0x100, {{"daxiq_channel", "1"}, {"daxiq_rate", "48000"}});
    check(!iq.stream(1).exists, "canceled late stream cannot flash the applet back On");
    events();
    check(f.backend->commands().count(QStringLiteral("stream create type=dax_iq daxiq_channel=1")) == 2,
        "explicit On/Off/On retries once after the canceled receipt settles");
    const size_t create = f.backend->writes.size() - 1;
    f.answer(create, 0x101);
    f.stream(0x101);
    iq.applyStreamStatus(0x101, {{"daxiq_channel", "1"}, {"daxiq_rate", "48000"}});
    check(iq.stream(1).exists && iq.stream(1).streamId == 0x101 && !iq.createPending(1), "only the current allocation becomes active");
    PanadapterModel* pan = f.publish(QStringLiteral("0x40000001"));
    check(f.radio.requestPanIqBinding(pan, 1) == ReceiveDispatch::Dispatched
        && f.backend->commands().last() == QStringLiteral("display pan set 0x40000001 daxiq_channel=1"),
        "pan IQ binding uses canonical display control");
    const AudioControlGuard first = f.guards.back();
    f.radio.requestPanIqBinding(pan, 0);
    check(!first.permits() && f.guards.back().permits(), "unbind revokes an older queued pan binding");
    const AudioControlGuard second = f.guards.back();
    pan->setClientHandle(QStringLiteral("72"));
    check(!second.permits() && f.radio.requestPanIqBinding(pan, 1) == ReceiveDispatch::Unsupported,
        "pan ownership change closes binding admission and its queued write");
    pan->setClientHandle(QStringLiteral("71"));
    f.radio.requestPanIqBinding(pan, 1);
    const AudioControlGuard third = f.guards.back();
    TxOperationIntegrationTestAccess::resetAudio(f.radio);
    check(!third.permits(), "session teardown revokes queued pan IQ binding");
}

void terminalWriter()
{
    AudioFixture f;
    QStringList writes;
    TxOperationIntegrationTestAccess::audioTransport(f.radio, f.identity, *f.backend,
        [&writes](quint32, const QString& command) { writes.append(command); });
    QThread worker;
    QThread* owner = QThread::currentThread();
    f.identity.moveToThread(&worker); // Not initialized: no socket exists.
    AudioStreamRequest cancelled{AudioStreamKind::Iq, AudioStreamAction::Create, 0, 1};
    cancelled.guard = AudioControlGuard::create(30000);
    f.radio.requestAudioStream(cancelled);
    cancelled.guard.revoke();
    f.radio.requestGraphicEq({GraphicEqRequest::Path::Receive, GraphicEqRequest::Field::Enabled, 0, 1});
    f.radio.requestGraphicEq({GraphicEqRequest::Path::Receive, GraphicEqRequest::Field::Enabled, 0, 0});
    PanadapterModel* pan = f.publish(QStringLiteral("0x40000001"));
    f.radio.requestPanIqBinding(pan, 1);
    pan->setClientHandle(QStringLiteral("72"));
    f.radio.requestAudioStream({AudioStreamKind::RemoteTransmit});
    QMetaObject::invokeMethod(&f.identity, [&f, owner, &worker] {
        f.identity.moveToThread(owner);
        worker.quit();
    }, Qt::QueuedConnection);
    worker.start();
    check(worker.wait(5000), "unopened transport worker drains within its deadline");
    events();
    check(writes == QStringList{QStringLiteral("eq RXsc mode=False"),
                               QStringLiteral("stream create type=remote_audio_tx compression=opus")},
        "production queued writer drops canceled creates, superseded EQ and retired pan binding");
}

void remoteRxAndNativeAudio()
{
    {
        AudioFixture f;
        TxOperationIntegrationTestAccess::startRx(f.radio);
        check(f.backend->commands().isEmpty() && TxOperationIntegrationTestAccess::pendingAudio(f.radio) == 0,
            "unregistered remote RX leaves no write or pending allocation");
    }
    for (bool statusFirst : {false, true}) {
        AudioFixture f;
        TxOperationIntegrationTestAccess::acceptGuiRegistration(f.radio);
        TxOperationIntegrationTestAccess::startRx(f.radio);
        check(!f.backend->writes.empty(), "registered remote RX starts its allocation");
        if (f.backend->writes.empty()) { continue; }
        const size_t create = f.backend->writes.size() - 1;
        if (statusFirst) { f.stream(0x100, QStringLiteral("remote_audio_rx")); }
        TxOperationIntegrationTestAccess::stopRx(f.radio);
        TxOperationIntegrationTestAccess::startRx(f.radio);
        f.answer(create, 0x100);
        events();
        check(f.backend->commands().filter(QStringLiteral("stream create type=remote_audio_rx")).size() == 2,
            "remote RX On/Off/On is bounded and survives status-before-reply");
    }
    RadioModel native;
    auto backend = std::make_unique<SimBackend>();
    SimBackend* source = backend.get();
    native.setBackendForTest(std::move(backend), QStringLiteral("sim"));
    check(!native.backendCapabilities().audioStreamControl && source->ownsRxAudio(),
        "native receive audio does not acquire Flex stream allocation capability");
    check(native.requestGraphicEq({GraphicEqRequest::Path::Receive, GraphicEqRequest::Field::Enabled, 0, 1})
        == ReceiveDispatch::LocalOnly, "host graphic EQ remains local without an unsupported wire command");
    check(native.requestAudioStream({AudioStreamKind::RemoteReceive}) == ReceiveDispatch::Unsupported,
        "native audio cannot fall through to Flex stream encoding");
    int requests = 0;
    QObject::connect(&native.equalizerModel(), &EqualizerModel::controlRequested, &native,
        [&requests](const GraphicEqRequest&) { ++requests; });
    native.equalizerModel().applyRxEqStatus({{"63Hz", "3"}, {"mode", "1"}});
    check(requests == 0, "radio EQ observation never feeds back a control request");
    std::thread offThread([&native] {
        native.equalizerModel().setRxBand(EqualizerModel::B63, 5);
        native.daxIqModel().setSampleRate(1, 96000);
        native.daxIqModel().removeStream(1);
    });
    offThread.join();
    check(requests == 0 && native.equalizerModel().rxBand(EqualizerModel::B63) == 3,
        "off-thread IQ and EQ mutations are refused before state changes");
    TransmitModel& tx = native.transmitModel();
    tx.setMicSelection(QStringLiteral("PC"));
    const quint64 revision = tx.micSelectionIntentRevision();
    tx.setMicSelection(QStringLiteral("PC"));
    check(tx.micSelectionIntentRevision() > revision, "same-value operator mic selection also invalidates a temporary restore");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("audio-control-routing"));
    qputenv("XDG_RUNTIME_DIR", profile.path().toUtf8());
    QCoreApplication app(argc, argv);
    encoding();
    identityAndReplies();
    cancellation();
    iqAndBinding();
    terminalWriter();
    remoteRxAndNativeAudio();
    return failures == 0 ? 0 : 1;
}
