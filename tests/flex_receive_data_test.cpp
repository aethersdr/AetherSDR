#include "core/backends/flex/FlexReceiveData.h"
#include <QCoreApplication>
#include <QThread>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool condition, const char* name)
{
    if (!condition) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
}
ReceiveSpectrumFrame frame(FlexReceiveData& data, quint32 id, float value)
{
    return {id, {value, value - 1}, {-140, -20, 17}, 123,
            ReceiveReceipt::now(), data.streamLease(id, false)};
}
ReceiveWaterfallRow row(FlexReceiveData& data, quint32 id, quint32 timecode)
{
    return {id, {42, 43}, 7.1, 7.2, timecode, timecode + 10, 123,
            ReceiveReceipt::now(), data.streamLease(id, true)};
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::vector<quint64> wakes;
    FlexReceiveData data([&](quint64 generation) { wakes.push_back(generation); });
    data.begin();
    constexpr quint32 pan = 0xf0000001;
    constexpr quint32 waterfall = 0xf2000001;
    data.registerStream(pan, false);
    data.registerStream(waterfall, true);
    data.spectrum(frame(data, pan, -100));
    data.spectrum(frame(data, pan, -90));
    data.waterfall(row(data, waterfall, 23));
    check(wakes.size() == 1, "one queued wake despite multiple packets");
    auto events = data.take(wakes.back());
    check(events.size() == 2, "latest spectrum plus independent waterfall");
    const auto spectrum = std::get<ReceiveSpectrumFrame>(events[0]);
    const auto history = std::get<ReceiveWaterfallRow>(events[1]);
    check(spectrum.streamId == pan && spectrum.binsDbm == QVector<float>({-90, -91})
          && spectrum.decodeScale.generation == 17 && spectrum.emittedNs == 123,
          "full-width identity, values and decode metadata survive");
    check(history.timecode == 23 && history.autoBlack == 33 && history.lowMhz == 7.1
          && history.intensity == QVector<float>({42, 43}), "waterfall observation is atomic");
    data.unregisterStream(pan, false);
    data.registerStream(pan, false);
    check(!spectrum.current(), "detached frame revoked on ID reuse");
    data.spectrum(spectrum);
    check(data.take(wakes.back()).empty(), "old incarnation cannot republish");

    MeterDef definition;
    definition.index = 5;
    definition.source = "SLC";
    definition.name = "LEVEL";
    definition.unit = "dBm";
    data.defineMeter(definition);
    data.meters({5}, {-12800}, ReceiveReceipt::now());
    events = data.take(wakes.back());
    const auto batch = std::get<ReceiveMeterBatch>(events[0]);
    check(batch.samples.size() == 1 && batch.samples[0].value == -100,
          "Flex normalization happens before the model");
    data.defineMeter(definition);
    check(batch.samples[0].definition->current(), "identical definition retains queued samples");
    definition.unit = "Volts";
    data.defineMeter(definition);
    check(!batch.samples[0].definition->current(), "unit change revokes old sample interpretation");
    check(FlexReceiveData::convertMeter("Volts", 256) == 1
          && FlexReceiveData::convertMeter("degC", 640) == 10
          && FlexReceiveData::convertMeter("Watts", 4) == 4, "wire scale parity");

    for (quint32 i = 0; i < 1000; ++i) { data.waterfall(row(data, waterfall, i)); }
    events = data.take(wakes.back());
    check(std::get<ReceiveWaterfallRow>(events.front()).timecode == 968,
          "waterfall overflow preserves a gap instead of retiming old history");
    check(data.stats().waterfallDropped == 968, "waterfall overload is counted");
    while (!data.take(wakes.back()).empty()) {}
    for (int i = 0; i < 1000; ++i) {
        data.meters({5}, {static_cast<qint16>(i)}, ReceiveReceipt::now());
    }
    events = data.take(wakes.back());
    check(std::get<ReceiveMeterBatch>(events.front()).samples[0].value == 872.0f / 256,
          "meter overflow evicts oldest whole batches");
    check(data.stats().meterBatchesDropped == 872, "meter overflow is distinct from wire loss");
    check(data.stats().queuedBytes <= FlexReceiveData::kMaxDisplayBytes + FlexReceiveData::kMaxMeterBytes,
          "buffer bytes remain capped");
    const quint64 oldWake = wakes.back();
    data.retire();
    check(!batch.current() && !history.current(), "retirement revokes all detached payloads");
    data.begin();
    data.registerStream(pan, false);
    data.spectrum(frame(data, pan, -80));
    check(data.take(oldWake).empty(), "old wake cannot consume the new session");
    check(data.take(wakes.back()).size() == 1, "new wake survives the old wake");

    ReceiveSpectrumFrame expiredFrame = frame(data, pan, -75);
    expiredFrame.receipt.monotonicNs -= FlexReceiveData::kMaxAgeNs + 1'000'000;
    data.spectrum(expiredFrame);
    check(data.take(wakes.back()).empty(), "queued frame age is based on receipt");
    ReceiveSpectrumFrame invalid = frame(data, pan, std::numeric_limits<float>::quiet_NaN());
    data.spectrum(invalid);
    check(data.take(wakes.back()).empty(), "nonfinite bins rejected");

    std::thread producer([&] {
        for (int i = 0; i < 10000; ++i) { data.spectrum(frame(data, pan, -60)); }
    });
    producer.join();
    events = data.take(wakes.back());
    check(events.size() == 1 && std::get<ReceiveSpectrumFrame>(events.front()).current(),
          "worker flood retains one current complete frame");
    data.close();
    check(!std::get<ReceiveSpectrumFrame>(events.front()).current(), "close revokes detached worker frame");

    FlexReceiveData bounded([&](quint64 generation) { wakes.push_back(generation); });
    // Capacity, not just used samples, counts toward retained memory.
    bounded.begin();
    bounded.registerStream(pan, false);
    ReceiveSpectrumFrame reserved = frame(bounded, pan, -65);
    reserved.binsDbm.reserve(3 * 1024 * 1024);
    bounded.spectrum(std::move(reserved));
    check(bounded.stats().queuedBytes == 0 && bounded.stats().rejected == 1,
          "oversized retained allocation cannot evade byte accounting");
    for (quint32 id = 1; id <= 64; ++id) {
        bounded.registerStream(id, true);
        ReceiveWaterfallRow large = row(bounded, id, id);
        large.intensity.fill(20, FlexReceiveData::kMaxBins);
        bounded.waterfall(std::move(large));
    }
    check(bounded.stats().waterfallDropped > 0
          && bounded.stats().peakQueuedBytes <= FlexReceiveData::kMaxDisplayBytes,
          "large display flood reaches the byte bound before the entry bound");
    bounded.begin();
    bounded.registerStream(pan, false);
    bounded.registerStream(waterfall, true);
    for (quint32 i = 0; i < 16; ++i) { bounded.waterfall(row(bounded, waterfall, i)); }
    bounded.defineMeter(definition);
    for (int i = 0; i < 32; ++i) { bounded.meters({5}, {256}, ReceiveReceipt::now()); }
    events = bounded.take(wakes.back(), receiveMonotonicNs());
    int displays = 0;
    int meters = 0;
    for (const auto& event : events) {
        if (std::holds_alternative<ReceiveMeterBatch>(event)) { ++meters; }
        else { ++displays; }
    }
    if (displays != 8 || meters != 8) { std::cerr << "fairness: display=" << displays << " meters=" << meters << '\n'; }
    check(displays == 8 && meters == 8, "one drain reserves service for both planes");
    while (!bounded.take(wakes.back()).empty()) {}
    const std::size_t beforeWake = wakes.size();
    bounded.spectrum(frame(bounded, pan, -60));
    check(wakes.size() == beforeWake + 1 && bounded.take(wakes.back()).size() == 1,
          "arrival after an empty drain cannot lose its wake");
    QVector<quint16> repeatedIds(16384, 5);
    QVector<qint16> repeatedValues(16384, 256);
    for (int i = 0; i < 8; ++i) { bounded.meters(repeatedIds, repeatedValues, ReceiveReceipt::now()); }
    check(bounded.stats().meterBatchesDropped > 0
          && bounded.stats().queuedBytes <= FlexReceiveData::kMaxMeterBytes,
          "large whole meter batches obey their separate byte budget");
    const auto retiredSession = bounded.sessionLease();
    bounded.begin();
    bounded.defineMeter(definition);
    bounded.meters({5}, {256}, ReceiveReceipt::now(), retiredSession);
    check(bounded.stats().queuedBytes == 0, "old-session meter producer cannot enter a replacement session");

    bounded.registerStream(pan, false);
    bounded.spectrum(frame(bounded, pan, -60));
    check(bounded.beginDrain(), "owner acquires drain reservation");
    const quint64 nestedGeneration = wakes.back();
    bounded.take(nestedGeneration);
    const std::size_t duringDrain = wakes.size();
    bounded.spectrum(frame(bounded, pan, -55));
    check(!bounded.beginDrain() && wakes.size() == duringDrain,
          "nested drain and producer wake stay deferred until delivery ends");
    bounded.retire();
    bounded.begin();
    bounded.registerStream(pan, false);
    bounded.spectrum(frame(bounded, pan, -50));
    check(wakes.size() == duringDrain, "reconnect preserves the active drain reservation");
    bounded.endDrain();
    check(wakes.size() == duringDrain + 1 && wakes.back() != nestedGeneration
          && bounded.take(wakes.back()).size() == 1,
          "ending the outer drain wakes the replacement session exactly once");

    // Race retirement with a producer that already captured its stream lease.
    bounded.registerStream(pan, false);
    const ReceiveSpectrumFrame racing = frame(bounded, pan, -70);
    std::thread retiringProducer([&] {
        for (int i = 0; i < 1000; ++i) { bounded.spectrum(racing); }
    });
    bounded.retire();
    retiringProducer.join();
    check(bounded.stats().queuedBytes == 0 && !racing.current(),
          "concurrent retirement fences a producer's captured lease");
    std::cout << "flex_receive_data_test: " << failures << " failures\n";
    return failures ? 1 : 0;
}
