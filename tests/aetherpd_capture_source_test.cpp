#include "AetherPDTestNetwork.h"
#include "core/txlinearity/AetherPDCaptureSource.h"
#include "core/txlinearity/ImdAnalyzer.h"
#include "core/txlinearity/TxLinearityController.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QThread>
#include <QtEndian>

#include <optional>
#include <array>
#include <bit>
#include <cmath>
#include <numbers>
#include <string>

using AetherSDR::txlin::AetherPDCaptureSource;
using AetherSDR::txlin::CaptureConfig;
using AetherSDR::txlin::CaptureFrame;

namespace {
class CaptureTestLease final : public AetherSDR::txlin::ITxLinearityRunLease {
public:
    bool start(std::int64_t deadline) override { cutoff = deadline; ++starts; observed = State::Keyed; return true; }
    void release() override { ++releases; }
    State state() const override { return observed; }
    State observed = State::AwaitingKey;
    int starts = 0;
    int releases = 0;
    std::int64_t cutoff = 0;
};
const QString kId = QStringLiteral("0123456789abcdef0123456789abcdef");
const QString kToken(43, u'a');
QByteArray encode(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
QJsonObject manifest()
{
    return {{"capture_id", kId}, {"format", "f32le-iq-v1"}, {"sample_rate_hz", 192000},
        {"center_hz", 7150000}, {"sample_count", 16384}, {"chunk_samples", 512},
        {"keyed", true}, {"correcting", true}, {"first_receive_ns", "1000000000"},
        {"last_receive_ns", "1085000000"}};
}
QJsonObject receiveManifest()
{
    QJsonObject result = manifest();
    result[QStringLiteral("keyed")] = false;
    result[QStringLiteral("correcting")] = false;
    return result;
}
QByteArray chunk(int index)
{
    QByteArray raw(4096, '\0');
    for (qsizetype i = 0; i < raw.size(); i += 8) {
        qToLittleEndian<quint32>(0x3f000000, raw.data() + i);
        qToLittleEndian<quint32>(0xbe800000, raw.data() + i + 4);
    }
    return encode({{"capture_id", kId}, {"offset", 512 * index},
                   {"iq_f32le", QString::fromLatin1(raw.toBase64())}});
}
CaptureConfig config()
{
    CaptureConfig result;
    result.centerFreqHz = 7150000;
    return result;
}
void setup(AetherPDCaptureSource& source, int& frames, int& errors)
{
    source.setFrameCallback([&frames](const CaptureFrame&) { ++frames; });
    source.setErrorCallback([&errors](const std::string&) { ++errors; });
    source.setSession(QHostAddress(QStringLiteral("192.168.50.100")), kToken, [] { return true; });
}
void completeCapture(Network& network)
{
    network.last->complete(200, encode(manifest()));
    for (int i = 0; i < 32; ++i) { network.captureChunk(i)->complete(200, chunk(i)); }
}

QByteArray twoToneChunk(int index)
{
    QByteArray raw(4096, '\0');
    for (int i = 0; i < 512; ++i) {
        const double t = double(index * 512 + i) / 192000.0;
        std::complex<double> sample{};
        for (const auto& [frequency, amplitude] : std::array<std::pair<double, double>, 4>{{
            {750, .2}, {1875, .2}, {-375, .002}, {3000, .002}}}) {
            sample += std::polar(amplitude, 2 * std::numbers::pi * frequency * t);
        }
        qToLittleEndian<quint32>(std::bit_cast<quint32>(float(sample.real())), raw.data() + 8 * i);
        qToLittleEndian<quint32>(std::bit_cast<quint32>(float(sample.imag())), raw.data() + 8 * i + 4);
    }
    return encode({{"capture_id", kId}, {"offset", 512 * index},
                   {"iq_f32le", QString::fromLatin1(raw.toBase64())}});
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    {
        using namespace AetherSDR::txlin;
        Network network;
        std::unique_ptr<AetherPDCaptureSource> source = std::make_unique<AetherPDCaptureSource>(network);
        source->setSession(QHostAddress(QStringLiteral("192.168.50.100")), kToken, [] { return true; });
        std::int64_t now = 1000;
        TxLinearityController::Environment environment;
        environment.nowMs = [&] { return now; };
        environment.sessionCurrent = [] { return true; };
        environment.telemetry = [](std::int64_t at) {
            TxTelemetry value;
            value.paTempC = {40.0, at}; value.supplyVolts = {13.8, at}; value.forwardWatts = {0.0, at};
            return value;
        };
        TxLinearityController controller(std::move(source), environment);
        int ready = 0, finished = 0;
        std::optional<LinearityRunResult> result;
        controller.setCallbacks([&] { ++ready; }, [&](const LinearityRunResult& value) { ++finished; result = value; });
        controller.confirmDummyLoad();
        LinearityRunConfig options;
        options.capture = config(); options.targetAverages = 1; options.maxForwardWatts = 10;
        RunRequest request;
        request.tone1Hz = 7150750; request.tone2Hz = 7151875; request.allocations = {{7000000, 7300000}};
        ok &= check(controller.prepare(options, request), "real adapter/controller prepare failed");
        network.last->complete(200, encode(receiveManifest()));
        for (int i = 0; i < 32; ++i) { network.captureChunk(i)->complete(200, chunk(i)); }
        ok &= check(ready == 1 && finished == 0 && network.sent.size() == 33,
                    "real receive readiness published measurement or started another capture");
        const std::shared_ptr<CaptureTestLease> lease = std::make_shared<CaptureTestLease>();
        ok &= check(controller.startRun(lease) && lease->starts == 1 && lease->cutoff == 11000,
                    "explicit bounded run did not start");
        now += 50; controller.poll();
        network.last->complete(200, encode(manifest()));
        for (int i = 0; i < 32; ++i) { network.captureChunk(i)->complete(200, twoToneChunk(i)); }
        QCoreApplication::processEvents();
        ok &= check(lease->releases == 1 && finished == 0 && network.sent.size() == 66,
                    "real adapter result escaped unkey confirmation or repeated after stop");
        lease->observed = CaptureTestLease::State::Released; controller.poll();
        ok &= check(finished == 1 && result && result->measurement && result->measurement->valid
                    && !result->measurement->clippingKnown && result->captures.size() == 1,
                    "real adapter/controller lost measurement or provenance");
        if (result && result->measurement) {
            for (const ImdProduct& product : result->measurement->products) {
                if (product.order == 3) {
                    ok &= check(std::abs(product.dbcPerTone + 40) < .1, "controller altered analytic IMD3");
                }
            }
        }
    }
    {
        Network network; AetherPDCaptureSource source(network); int frames = 0, errors = 0, ready = 0;
        setup(source, frames, errors);
        ok &= check(!source.checkReceiveReady([&] { ++ready; }) && network.sent.empty(),
                    "readiness accepted before start");
        ok &= check(source.start(config()), "readiness fixture start failed");
        ok &= check(!source.checkReceiveReady({}) && network.sent.empty(), "missing readiness callback accepted");
        ok &= check(source.checkReceiveReady([&] { ++ready; }), "readiness check refused");
        ok &= check(!source.checkReceiveReady([&] { ++ready; }), "overlapping readiness accepted");
        network.last->complete(200, encode(receiveManifest()));
        for (int i = 0; i < 31; ++i) { network.captureChunk(i)->complete(200, chunk(i)); }
        ok &= check(ready == 0 && frames == 0 && errors == 0, "partial IQ granted readiness");
        network.captureChunk(31)->complete(200, chunk(31));
        QCoreApplication::processEvents();
        ok &= check(ready == 1 && frames == 0 && errors == 0 && network.sent.size() == 33,
                    "readiness published a frame, repeated or did not complete");
        for (const Network::Sent& sent : network.sent) {
            ok &= check(sent.body.isEmpty() && sent.request.url().path().startsWith(QStringLiteral("/v1/aetherpd/capture")),
                        "receive readiness issued a control request");
        }
        source.setGateOpen(true);
        ok &= check(!source.checkReceiveReady([&] { ++ready; }), "readiness accepted inside transmit gate");
        completeCapture(network);
        source.stop();
        ok &= check(ready == 1 && frames == 1 && errors == 0, "ready source could not capture a subsequent keyed frame");
    }
    for (const int action : {0, 1, 2, 3}) {
        Network network; AetherPDCaptureSource source(network); int frames = 0, errors = 0, ready = 0;
        setup(source, frames, errors);
        bool current = true;
        source.setSession(QHostAddress(QStringLiteral("192.168.50.100")), kToken, [&] { return current; });
        ok &= check(source.start(config()) && source.checkReceiveReady([&] { ++ready; }), "cancellation fixture failed");
        network.last->complete(200, encode(receiveManifest()));
        for (int i = 0; i < 31; ++i) { network.captureChunk(i)->complete(200, chunk(i)); }
        QPointer<Reply> old = network.last;
        if (action == 0) { source.stop(); }
        if (action == 1) { source.setGateOpen(false); }
        if (action == 2) { source.setSession(QHostAddress(QStringLiteral("192.168.50.100")), kToken, [] { return true; }); }
        if (action == 3) { current = false; }
        old->complete(200, chunk(31));
        ok &= check(ready == 0 && frames == 0 && errors == (action == 3 ? 1 : 0),
                    "canceled or stale readiness survived final reply");
        if (action != 3) { ok &= check(old->aborted, "readiness cancellation left HTTP request active"); }
        source.stop();
    }
    for (const int fault : {0, 1, 2, 3, 4, 5}) {
        Network network; AetherPDCaptureSource source(network); int frames = 0, errors = 0, ready = 0;
        setup(source, frames, errors);
        ok &= check(source.start(config()) && source.checkReceiveReady([&] { ++ready; }), "readiness fault fixture failed");
        QJsonObject body = receiveManifest();
        if (fault == 0) { body = manifest(); }
        if (fault == 1) { body[QStringLiteral("sample_rate_hz")] = 96000; }
        if (fault == 2) { body[QStringLiteral("center_hz")] = 7160000; }
        if (fault == 3) { network.last->complete(401, "{}"); }
        else if (fault == 4) {
            const QList<QTimer*> timers = source.findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly);
            ok &= check(timers.size() == 1 && timers[0]->isActive(), "readiness deadline not armed");
            if (!timers.empty()) { QMetaObject::invokeMethod(timers[0], "timeout", Qt::DirectConnection); }
        } else { network.last->complete(200, encode(body)); }
        if (fault == 5) {
            for (int i = 0; i < 31; ++i) { network.captureChunk(i)->complete(200, chunk(i)); }
            network.captureChunk(31)->complete(200, chunk(30));
        }
        QCoreApplication::processEvents();
        ok &= check(ready == 0 && frames == 0 && errors == 1, "invalid or incomplete receive snapshot granted readiness");
        ok &= check(!source.checkReceiveReady([&] { ++ready; }), "failed readiness remained running");
    }
    {
        Network network;
        std::unique_ptr<AetherPDCaptureSource> source = std::make_unique<AetherPDCaptureSource>(network);
        int frames = 0, errors = 0, ready = 0; setup(*source, frames, errors);
        ok &= check(source->start(config()) && source->checkReceiveReady([&] { ++ready; source.reset(); }),
                    "readiness destruction fixture failed");
        network.last->complete(200, encode(receiveManifest()));
        for (int i = 0; i < 32; ++i) { network.captureChunk(i)->complete(200, chunk(i)); }
        QCoreApplication::processEvents();
        ok &= check(!source && ready == 1 && frames == 0 && errors == 0 && network.sent.size() == 33,
                    "readiness callback destruction revived source");
    }
    {
        Network network; AetherPDCaptureSource source(network); int frames = 0, errors = 0;
        setup(source, frames, errors);
        source.setFrameCallback([&](const CaptureFrame& frame) {
            using namespace AetherSDR::txlin;
            TxSpectrumAnalyzer analyzer(16384, WindowKind::BlackmanHarris7);
            ok &= check(analyzer.accumulate(frame.feedback), "downloaded IQ rejected by analyzer");
            ImdConfig options; options.maxOrder = 3;
            const LinearityResult result = measureTwoTone(analyzer, frame.sampleRateHz, options, frame.clipped, frame.clippingKnown);
            ok &= check(result.valid && !result.clippingKnown && result.products.size() == 2,
                        "downloaded two-tone measurement invalid or overload uncertainty lost");
            for (const ImdProduct& product : result.products) {
                ok &= check(std::abs(product.dbcPerTone + 40) < .1, "downloaded IMD3 differs from analytic -40 dBc");
            }
            ++frames; source.stop();
        });
        ok &= check(source.start(config()), "analyzer fixture start failed"); source.setGateOpen(true);
        network.last->complete(200, encode(manifest()));
        ok &= check(network.sent.size() == 33, "all bounded chunk requests must be queued before any completes");
        for (int i = 31; i >= 0; --i) { network.captureChunk(i)->complete(200, twoToneChunk(i)); }
        ok &= check(frames == 1 && errors == 0, "analyzer did not receive one complete frame");
    }
    {
        Network network;
        AetherPDCaptureSource source(network);
        int frames = 0, errors = 0;
        setup(source, frames, errors);
        std::optional<CaptureFrame> captured;
        source.setFrameCallback([&](const CaptureFrame& frame) { captured = frame; ++frames; source.stop(); });
        ok &= check(source.start(config()) && network.sent.empty(), "start issued a request before gate opened");
        source.setGateOpen(true);
        ok &= check(network.sent.size() == 1 && network.sent[0].method == QNetworkAccessManager::PostOperation
                    && network.sent[0].request.url().path() == QStringLiteral("/v1/aetherpd/capture"), "wrong begin request");
        completeCapture(network);
        ok &= check(frames == 1 && errors == 0 && captured && captured->feedback.size() == 16384, "complete frame not delivered");
        ok &= check(captured && !captured->reference && !captured->clippingKnown
                    && captured->sourceCaptureId == kId.toStdString() && captured->correctionActive == true
                    && captured->timestampNs == 1000000000 && captured->sampleRateHz == 192000
                    && captured->centerFreqHz == 7150000, "capture provenance lost");
        if (captured) {
            for (const std::complex<float>& sample : captured->feedback) {
                ok &= check(sample == std::complex<float>(0.5f, -0.25f), "raw IQ altered");
            }
        }
        for (std::size_t i = 0; i < network.sent.size(); ++i) {
            const Network::Sent& sent = network.sent[i];
            ok &= check(sent.body.isEmpty() && sent.request.rawHeader("Authorization") == "Bearer " + kToken.toLatin1()
                && sent.request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt() == QNetworkRequest::ManualRedirectPolicy,
                "credential/body/redirect policy changed");
            if (i > 0) {
                ok &= check(sent.method == QNetworkAccessManager::GetOperation
                    && sent.request.url().path() == QStringLiteral("/v1/aetherpd/capture/%1/%2").arg(kId).arg(i - 1),
                    "capture sent a non-capture operation or wrong chunk");
            }
        }
        QCoreApplication::processEvents();
        ok &= check(network.sent.size() == 33, "callback stop started another capture");
    }
    for (const int action : {0, 1, 2}) {
        Network network; AetherPDCaptureSource source(network); int frames = 0, errors = 0;
        setup(source, frames, errors);
        ok &= check(source.start(config()), "fixture start failed"); source.setGateOpen(true);
        network.last->complete(200, encode(manifest()));
        network.captureChunk(0)->complete(200, chunk(0));
        QPointer<Reply> old = network.last;
        if (action == 0) { source.stop(); }
        if (action == 1) { source.setGateOpen(false); }
        if (action == 2) { source.setSession(QHostAddress(QStringLiteral("192.168.50.100")), kToken, [] { return true; }); }
        ok &= check(old && old->aborted, "stop/gate/session change did not abort pending request");
        old->complete(200, chunk(1));
        ok &= check(frames == 0 && errors == 0 && network.sent.size() == 33, "late reply escaped old capture");
        if (action == 1) {
            source.setGateOpen(true);
            ok &= check(network.sent.back().method == QNetworkAccessManager::PostOperation, "gate reopen reused partial capture");
        }
    }
    for (const int fault : {0, 1, 2, 3, 4, 5}) {
        Network network; AetherPDCaptureSource source(network); int frames = 0, errors = 0;
        setup(source, frames, errors); bool current = true;
        source.setSession(QHostAddress(QStringLiteral("192.168.50.100")), kToken, [&] { return current; });
        ok &= check(source.start(config()), "fault fixture start failed"); source.setGateOpen(true);
        QJsonObject body = manifest();
        if (fault == 0) { body[QStringLiteral("keyed")] = false; body[QStringLiteral("correcting")] = false; }
        if (fault == 1) { body[QStringLiteral("sample_rate_hz")] = 96000; }
        if (fault == 2) { body[QStringLiteral("center_hz")] = 7160000; }
        if (fault == 3) { current = false; }
        if (fault == 4) { network.last->complete(401, "{}"); }
        else if (fault == 5) { network.last->complete(200, QByteArray(8193, 'x')); }
        else { network.last->complete(200, encode(body)); }
        ok &= check(frames == 0 && errors == 1 && network.sent.size() == 1, "invalid capture retried or reached analyzer");
    }
    {
        Network network; AetherPDCaptureSource source(network); int frames = 0, errors = 0;
        setup(source, frames, errors);
        source.setSession(QHostAddress(QStringLiteral("8.8.8.8")), kToken, [] { return true; });
        ok &= check(!source.start(config()) && network.sent.empty(), "public destination accepted");
        source.setSession(QHostAddress(QStringLiteral("192.168.50.100")), "bad", [] { return true; });
        ok &= check(!source.start(config()) && network.sent.empty(), "invalid credential accepted");
        setup(source, frames, errors);
        ok &= check(source.start(config()), "timeout fixture start failed"); source.setGateOpen(true);
        const QList<QTimer*> timers = source.findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly);
        ok &= check(timers.size() == 1 && timers[0]->isActive() && timers[0]->isSingleShot()
                    && timers[0]->interval() == 6000, "capture deadline not armed");
        ok &= check(QMetaObject::invokeMethod(timers[0], "timeout", Qt::DirectConnection), "deadline could not be injected");
        ok &= check(errors == 1 && frames == 0 && network.last->aborted, "deadline left request active");
    }
    {
        std::unique_ptr<Network> network = std::make_unique<Network>();
        AetherPDCaptureSource source(*network); int frames = 0, errors = 0;
        setup(source, frames, errors); ok &= check(source.start(config()), "transport-loss fixture start failed");
        source.setGateOpen(true); network.reset();
        ok &= check(frames == 0 && errors == 1, "transport loss did not terminate capture");
    }
    {
        Network network;
        std::unique_ptr<AetherPDCaptureSource> source = std::make_unique<AetherPDCaptureSource>(network);
        int frames = 0, errors = 0; setup(*source, frames, errors);
        source->setFrameCallback([&](const CaptureFrame&) { ++frames; source.reset(); });
        ok &= check(source->start(config()), "destruction fixture start failed"); source->setGateOpen(true);
        completeCapture(network); QCoreApplication::processEvents();
        ok &= check(!source && frames == 1 && errors == 0 && network.sent.size() == 33, "callback destruction revived source");
    }
    for (int fault = 0; fault < 4; ++fault) {
        Network network;
        AetherPDCaptureSource source(network); int frames = 0, errors = 0;
        setup(source, frames, errors);
        ok &= check(source.setMeasurementSource(31) && source.start(config()), "measurement source binding refused");
        source.setGateOpen(true);
        ok &= check(network.sent.back().request.url().path().endsWith("/status"), "capture began before source observation");
        QJsonObject status{{"available", true}, {"registered", true}, {"keyed", true},
            {"correction_enabled", true}, {"correcting", false}, {"state", "calibrating"},
            {"calibration_required", false}, {"calibration_control", true}, {"calibration_state", "running"},
            {"calibration_samples_left", 90000}, {"input_limited_samples", 0},
            {"measurement_control", true}, {"source_purpose", "measurement"},
            {"measurement", QJsonObject{{"source_id", "000000000000001f"}, {"tones", false}}},
            {"faults", QJsonObject{{"dispatch_deadlines",0},{"send_errors",0},{"rejected_input",0},{"stale_iq",0}}}};
        network.last->complete(200, encode(status));
        ok &= check(network.sent.size() == 1 && frames == 0 && errors == 0, "preamble admitted a capture");
        if (fault == 3) {
            source.stop();
            QThread::msleep(25); QCoreApplication::processEvents();
            ok &= check(network.sent.size() == 1 && frames == 0, "delayed tone poll survived Stop");
            continue;
        }
        QThread::msleep(25); QCoreApplication::processEvents();
        status["measurement"] = QJsonObject{{"source_id", "000000000000001f"}, {"tones", true}};
        network.last->complete(200, encode(status));
        ok &= check(network.sent.back().request.url().path().endsWith("/capture"), "ready source did not start capture");
        QJsonObject captured = manifest();
        captured["measurement"] = QJsonObject{{"source_id", fault == 1 ? "0000000000000020" : "000000000000001f"}, {"tones", fault != 2}};
        network.last->complete(200, encode(captured));
        if (fault) {
            ok &= check(errors == 1 && frames == 0, "changed source or mixed tone frame admitted");
        } else {
            for (int chunkIndex = 0; chunkIndex < 32; ++chunkIndex) { network.captureChunk(chunkIndex)->complete(200, chunk(chunkIndex)); }
            ok &= check(frames == 1 && errors == 0, "matching steady-source capture missing");
        }
        source.stop();
    }
    for (int fault = 0; fault < 5; ++fault) {
        Network network; AetherPDCaptureSource source(network); int frames = 0, errors = 0;
        setup(source, frames, errors);
        ok &= check(source.start(config()), "parallel fault fixture start failed");
        source.setGateOpen(true);
        network.last->complete(200, encode(manifest()));
        ok &= check(network.sent.size() == 33, "parallel snapshot has wrong request bound");
        std::array<QPointer<Reply>, 32> pending;
        for (int i = 0; i < 32; ++i) { pending[i] = network.captureChunk(i); }
        if (!pending[17]) { return 1; }
        pending[17]->complete(200, chunk(17));
        QMetaObject::invokeMethod(pending[17], "finished", Qt::DirectConnection);
        ok &= check(frames == 0 && errors == 0, "duplicate completion advanced a partial capture");
        if (fault == 0) { pending[9]->complete(200, chunk(10)); }
        if (fault == 1) { pending[9]->complete(401, "{}"); }
        if (fault == 2) { pending[9]->complete(200, QByteArray(8193, 'x')); }
        if (fault == 3) {
            const QList<QTimer*> timers = source.findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly);
            QMetaObject::invokeMethod(timers[0], "timeout", Qt::DirectConnection);
        }
        if (fault == 4) { source.stop(); }
        const int expectedErrors = fault == 4 ? 0 : 1;
        for (int i = 0; i < 32; ++i) {
            if (i != 17 && (i != 9 || fault >= 3)) {
                ok &= check(pending[i] && pending[i]->aborted, "fault left another queued chunk active");
            }
            if (pending[i]) { QMetaObject::invokeMethod(pending[i], "finished", Qt::DirectConnection); }
        }
        QCoreApplication::processEvents();
        ok &= check(frames == 0 && errors == expectedErrors && network.sent.size() == 33,
                    "parallel fault emitted a frame, repeated error, or retried");
    }
    return ok ? 0 : 1;
}
