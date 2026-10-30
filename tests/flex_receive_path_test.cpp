#include "TestSettingsProfile.h"
#include "SeamThreadAffinityProbe.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/FlexReceiveData.h"
#include "core/backends/flex/PanadapterStream.h"
#include "models/RadioModel.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonObject>
#include <QSignalSpy>
#include <QThread>
#include <QtEndian>
#include <cmath>
#include <iostream>

using namespace AetherSDR;
namespace AetherSDR {
class FlexReceiveDataTestAccess {
public:
    static void begin(FlexBackend& backend) { backend.m_receiveData->begin(); }
    static void fft(PanadapterStream& stream, quint32 id, quint32 sequence)
    {
        QByteArray packet(48, '\0');
        auto* raw = reinterpret_cast<uchar*>(packet.data());
        qToBigEndian<quint16>(4, raw + 30);
        qToBigEndian<quint16>(2, raw + 32);
        qToBigEndian<quint16>(4, raw + 34);
        qToBigEndian<quint32>(sequence, raw + 36);
        for (int i = 0; i < 4; ++i) { qToBigEndian<quint16>(350, raw + 40 + 2 * i); }
        stream.decodeFFT(raw, packet.size(), false, id);
    }
    static void waterfall(PanadapterStream& stream, quint32 id, quint32 sequence)
    {
        QByteArray packet(72, '\0');
        auto* raw = reinterpret_cast<uchar*>(packet.data());
        qToBigEndian<qint64>(-524288000LL, raw + 28); // -500 Hz near-DC overhang
        qToBigEndian<qint64>(393216000LL, raw + 36); // 375 Hz/bin, FlexLib VitaFrequency
        qToBigEndian<quint32>(50, raw + 44);
        qToBigEndian<quint16>(4, raw + 48);
        qToBigEndian<quint16>(1, raw + 50);
        qToBigEndian<quint32>(sequence, raw + 52);
        qToBigEndian<quint32>(12345, raw + 56);
        qToBigEndian<quint16>(4, raw + 60);
        for (int i = 0; i < 4; ++i) { qToBigEndian<qint16>(12800 + i * 128, raw + 64 + 2 * i); }
        stream.decodeWaterfallTile(raw, packet.size(), false, id);
    }
    static void meters(PanadapterStream& stream)
    {
        QByteArray packet(36, '\0');
        auto* raw = reinterpret_cast<uchar*>(packet.data());
        qToBigEndian<quint16>(5, raw + 28);
        qToBigEndian<qint16>(-12800, raw + 30);
        qToBigEndian<quint16>(6, raw + 32);
        qToBigEndian<qint16>(3200, raw + 34);
        stream.decodeMeterData(raw, packet.size(), false);
    }
    static void telemetry(FlexBackend& backend)
    {
        ReceiveTelemetry snapshot;
        snapshot.lease = backend.m_receiveData->sessionLease();
        snapshot.receipt = ReceiveReceipt::now();
        snapshot.rxBytes = 4096;
        snapshot.packets = 50;
        snapshot.errors = 2;
        snapshot.firstPacketSeen = true;
        snapshot.localEndpoint = "192.0.2.1:50000";
        snapshot.categories[ReceiveStreamStats::Spectrum] = {2048, 20, 1};
        backend.m_receiveData->telemetry(snapshot);
    }
};
}
namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::cout << (condition ? "PASS: " : "FAIL: ") << message << '\n';
    if (!condition) { ++failures; }
}
void drain() { for (int i = 0; i < 4; ++i) { QCoreApplication::processEvents(QEventLoop::AllEvents); } }
void nestedDelivery()
{
    RadioModel radio;
    auto* backend = dynamic_cast<FlexBackend*>(radio.backend());
    PanadapterStream* stream = backend->panStream();
    FlexReceiveDataTestAccess::begin(*backend);
    stream->registerPanStream(1); stream->registerWfStream(2);
    QSignalSpy rows(&radio, &RadioModel::panFeedWaterfallRowReady);
    const auto produce = [&](quint32 sequence) {
        QMetaObject::invokeMethod(stream, [&, sequence] {
            FlexReceiveDataTestAccess::fft(*stream, 1, sequence);
            FlexReceiveDataTestAccess::waterfall(*stream, 2, sequence);
        }, Qt::BlockingQueuedConnection);
    };
    bool nested = false;
    QObject::connect(&radio, &RadioModel::panFeedSpectrumReady, &radio, [&] {
        if (!nested) { nested = true; produce(2); drain(); }
    });
    produce(1); drain();
    check(rows.size() == 2 && rows[0][4].toUInt() == 1 && rows[1][4].toUInt() == 2,
          "nested event loop cannot deliver newer waterfall rows before the detached batch");
}

void meterClock()
{
    MeterModel model;
    MeterDef def; def.index = 5; def.source = "SLC"; def.sourceIndex = 0;
    def.name = "LEVEL"; def.unit = "dBm";
    model.defineMeter(def);
    auto session = std::make_shared<ReceiveDataEpoch>();
    auto definition = std::make_shared<ReceiveDataEpoch>(); definition->session = session;
    ReceiveMeterBatch batch{{{5, -85.5f, definition}}, ReceiveReceipt::now(), session};
    batch.receipt.monotonicNs -= 300'000'000;
    batch.receipt.utcMs -= 300;
    model.updateReceivedValues(batch);
    check(model.value(5) == -85.5f && model.valueAgeMs(5) >= 300
          && model.valueUpdatedAtMs(5) == batch.receipt.utcMs,
          "delivery keeps receipt age and public UTC timestamp");
    check(model.allMeters()[0].toObject().value("age_ms").toInt() >= 300,
          "snapshot age agrees with the scalar age");
    const qint64 originalUtc = model.valueUpdatedAtMs(5);
    ReceiveMeterBatch late = batch;
    late.receipt.monotonicNs -= 10'000'000;
    late.samples[0].value = -50;
    model.updateReceivedValues(late);
    check(model.value(5) == -85.5f && model.valueUpdatedAtMs(5) == originalUtc,
          "out-of-order delivery does not overwrite a newer observation");
    batch.receipt = ReceiveReceipt::now();
    batch.receipt.utcMs -= 24 * 60 * 60 * 1000;
    model.updateReceivedValues(batch);
    check(model.valueAgeMs(5) >= 0 && model.valueAgeMs(5) < 100,
          "freshness uses monotonic receipt time despite a wall-clock correction");
    definition->active.store(false);
    batch.samples[0].value = -40;
    model.updateReceivedValues(batch);
    check(model.value(5) == -85.5f, "retired meter definition cannot update model");
    definition = std::make_shared<ReceiveDataEpoch>(); definition->session = session;
    batch = {{{5, -80, definition}, {6, 55, definition}}, ReceiveReceipt::now(), session};
    MeterDef temp; temp.index = 6; temp.source = "RAD"; temp.name = "PATEMP"; temp.unit = "degC";
    model.defineMeter(temp);
    QObject::connect(&model, &MeterModel::meterUpdated, &model, [&](int id, float) {
        if (id == 5) { model.clear(); }
    });
    model.updateReceivedValues(batch);
    check(model.allMeters().empty() && model.valueAgeMs(6) == -1,
          "clear inside a meter callback cannot resurrect the rest of the batch");

}
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile("flex-receive-path");
    QCoreApplication app(argc, argv);
    nestedDelivery();
    meterClock();
    RadioModel radio;
    auto* backend = dynamic_cast<FlexBackend*>(radio.backend());
    check(backend != nullptr, "production Flex backend available");
    if (!backend) { return 1; }
    PanadapterStream* stream = backend->panStream();
    FlexReceiveDataTestAccess::begin(*backend);
    test::SeamThreadAffinityProbe affinity(backend);
    test::attachAllSeamSignals(affinity);
    QSignalSpy spectrum(&radio, &RadioModel::panFeedSpectrumReady);
    QSignalSpy waterfall(&radio, &RadioModel::panFeedWaterfallRowReady);
    QSignalSpy black(&radio, &RadioModel::panFeedWaterfallAutoBlackLevel);
    QSignalSpy ping(&radio, &RadioModel::pingReceived);
    QSignalSpy legacySpectrum(stream, &PanadapterStream::spectrumReady);
    QSignalSpy legacyWaterfall(stream, &PanadapterStream::waterfallRowReady);
    QSignalSpy legacyMeters(stream, &PanadapterStream::meterDataReady);
    constexpr quint32 pan = 0xf0000001;
    constexpr quint32 wf = 0xf2000001;
    stream->registerPanStream(pan);
    stream->registerWfStream(wf);
    backend->decodeMeterStatus("5.src=SLC#5.num=0#5.nam=LEVEL#5.unit=dBm#6.src=RAD#6.nam=PATEMP#6.unit=degC");
    const auto produce = [&](quint32 sequence) {
        QMetaObject::invokeMethod(stream, [&, sequence] {
            FlexReceiveDataTestAccess::fft(*stream, pan, sequence);
            FlexReceiveDataTestAccess::waterfall(*stream, wf, sequence);
            FlexReceiveDataTestAccess::meters(*stream);
        }, Qt::BlockingQueuedConnection);
    };
    produce(1);
    check(spectrum.empty() && waterfall.empty(), "worker does not emit model or seam frames directly");
    drain();
    check(spectrum.size() == 1 && waterfall.size() == 1 && black.size() == 1,
          "one actual decoder delivery per plane, with no synthesized duplicate");
    check(legacySpectrum.empty() && legacyWaterfall.empty() && legacyMeters.empty(),
          "production Flex no longer publishes parallel raw outlets");
    if (!spectrum.empty()) {
        const QVector<float> bins = qvariant_cast<QVector<float>>(spectrum[0][1]);
        const SpectrumDecodeScale scale = qvariant_cast<SpectrumDecodeScale>(spectrum[0][3]);
        check(spectrum[0][0].toUInt() == pan && bins.size() == 4 && std::abs(bins[0] + 85.0644f) < .001f
              && scale.valid(), "FFT calibration and full-width identity preserved");
    }
    if (!waterfall.empty()) {
        check(waterfall[0][0].toUInt() == wf && waterfall[0][2].toDouble() < 0
              && qvariant_cast<QVector<float>>(waterfall[0][1]) == QVector<float>({100, 101, 102, 103})
              && black[0][1].toUInt() == 12345, "signed near-DC waterfall and black level preserved");
    }
    check(radio.meterModel().value(5) == -100 && radio.meterModel().value(6) == 50,
          "wire meter batch is normalized once and reaches model");
    FlexReceiveDataTestAccess::telemetry(*backend);
    drain();
    check(radio.packetTotalCount() == 50 && radio.packetDropCount() == 2
          && radio.categoryStats(ReceiveStreamStats::Spectrum).bytes == 2048,
          "diagnostics read backend snapshot");
    check(ping.empty(), "UDP telemetry is not a TCP ping confirmation");
    produce(2);
    stream->unregisterPanStream(pan); stream->registerPanStream(pan);
    stream->unregisterWfStream(wf); stream->registerWfStream(wf);
    drain();
    check(spectrum.size() == 1 && waterfall.size() == 1, "queued data fenced across ID reuse");
    produce(3);
    backend->retireReceiveData();
    drain();
    check(spectrum.size() == 1 && waterfall.size() == 1, "queued data fenced across session retirement");
    check(radio.packetTotalCount() == 50 && !radio.firstUdpPacketSeen()
          && radio.localUdpEndpoint() == "Not bound", "retirement preserves lifetime totals, clears live endpoint");
    FlexReceiveDataTestAccess::begin(*backend);
    stream->registerPanStream(pan); stream->registerWfStream(wf);
    QObject::connect(&radio, &RadioModel::panFeedSpectrumReady, &radio,
        [&] { backend->retireReceiveData(); });
    produce(4);
    drain();
    check(spectrum.size() == 2 && waterfall.size() == 1,
          "disconnect inside one delivery cancels remaining detached events");
    emit backend->disconnected();
    drain();
    check(affinity.violations().empty() && affinity.afterDisconnect().empty()
          && affinity.count("receiveSpectrumReady") >= 2
          && affinity.count("receiveWaterfallReady") >= 1
          && affinity.count("receiveMetersReady") >= 1,
          "non-vacuous Flex owner-thread and disconnect contract");
    std::cout << "flex_receive_path_test: " << failures << " failures\n";
    return failures ? 1 : 0;
}
