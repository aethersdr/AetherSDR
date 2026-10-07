// Socket-free production model/encoder and routing checks. No radio peer or TX.
#include "TestSettingsProfile.h"
#include "core/backends/flex/FlexBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"
#include <QCoreApplication>
#include <QSignalSpy>
#include <QStringList>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <limits>
#include <vector>

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}
struct TxCase {
    const char* name;
    std::function<void(TransmitModel&)> apply;
    QStringList expected;
};
void transmitEncoding()
{
    const std::vector<TxCase> cases{
        {"setRfPower", [](TransmitModel& model) { model.setRfPower(42); }, {QStringLiteral("transmit set rfpower=42")}},
        {"setTunePower", [](TransmitModel& model) { model.setTunePower(11); }, {QStringLiteral("transmit set tunepower=11")}},
        {"setTuneMode", [](TransmitModel& model) { model.setTuneMode(QStringLiteral("two_tone")); }, {QStringLiteral("transmit set tune_mode=two_tone")}},
        {"setMicSelection", [](TransmitModel& model) { model.setMicSelection(QStringLiteral("BAL")); }, {QStringLiteral("mic input BAL")}},
        {"setMicLevel", [](TransmitModel& model) { model.setMicLevel(34); }, {QStringLiteral("transmit set miclevel=34")}},
        {"setMicAcc", [](TransmitModel& model) { model.setMicAcc(true); }, {QStringLiteral("mic acc 1")}},
        {"setSpeechProcessorEnable", [](TransmitModel& model) { model.setSpeechProcessorEnable(true); }, {QStringLiteral("transmit set speech_processor_enable=1")}},
        {"setSpeechProcessorLevel", [](TransmitModel& model) { model.setSpeechProcessorLevel(1); }, {QStringLiteral("transmit set speech_processor_level=1")}},
        {"setDax", [](TransmitModel& model) { model.setDax(true); }, {QStringLiteral("transmit set dax=1")}},
        {"setSbMonitor", [](TransmitModel& model) { model.setSbMonitor(true); }, {QStringLiteral("transmit set mon=1")}},
        {"setMonGainSb", [](TransmitModel& model) { model.setMonGainSb(35); }, {QStringLiteral("transmit set mon_gain_sb=35")}},
        {"setVoxEnable", [](TransmitModel& model) { model.setVoxEnable(true); }, {QStringLiteral("transmit set vox_enable=1")}},
        {"setVoxLevel", [](TransmitModel& model) { model.setVoxLevel(26); }, {QStringLiteral("transmit set vox_level=26")}},
        {"setVoxDelay", [](TransmitModel& model) { model.setVoxDelay(27); }, {QStringLiteral("transmit set vox_delay=27")}},
        {"setMicBoost", [](TransmitModel& model) { model.setMicBoost(true); }, {QStringLiteral("mic boost 1")}},
        {"setMicBias", [](TransmitModel& model) { model.setMicBias(true); }, {QStringLiteral("mic bias 1")}},
        {"setAmCarrierLevel", [](TransmitModel& model) { model.setAmCarrierLevel(28); }, {QStringLiteral("transmit set am_carrier=28")}},
        {"setDexp", [](TransmitModel& model) { model.setDexp(true); }, {QStringLiteral("transmit set compander=1")}},
        {"setDexpLevel", [](TransmitModel& model) { model.setDexpLevel(29); }, {QStringLiteral("transmit set compander_level=29")}},
        {"setTxFilter", [](TransmitModel& model) { model.setTxFilter(200, 2800); }, {QStringLiteral("transmit set filter_low=200 filter_high=2800")}},
        {"setCwSpeed", [](TransmitModel& model) { model.setCwSpeed(25); }, {QStringLiteral("cw wpm 25")}},
        {"setCwPitch", [](TransmitModel& model) { model.setCwPitch(700); }, {QStringLiteral("cw pitch 700")}},
        {"setCwBreakIn", [](TransmitModel& model) { model.setCwBreakIn(true); }, {QStringLiteral("cw break_in 1")}},
        {"setCwDelay", [](TransmitModel& model) { model.setCwDelay(300); }, {QStringLiteral("cw break_in_delay 300")}},
        {"setCwSidetone", [](TransmitModel& model) { model.setCwSidetone(true); }, {QStringLiteral("cw sidetone 1")}},
        {"setCwIambic", [](TransmitModel& model) { model.setCwIambic(true); }, {QStringLiteral("cw iambic 1")}},
        {"setCwIambicMode", [](TransmitModel& model) { model.setCwIambicMode(1); }, {QStringLiteral("cw mode 1")}},
        {"setCwSwapPaddles", [](TransmitModel& model) { model.setCwSwapPaddles(true); }, {QStringLiteral("cw swap 1")}},
        {"setCwlEnabled", [](TransmitModel& model) { model.setCwlEnabled(true); }, {QStringLiteral("cw cwl_enabled 1")}},
        {"setMonGainCw", [](TransmitModel& model) { model.setMonGainCw(34); }, {QStringLiteral("transmit set mon_gain_cw=34")}},
        {"setMonPanCw", [](TransmitModel& model) { model.setMonPanCw(35); }, {QStringLiteral("transmit set mon_pan_cw=35")}},
        {"loadProfile", [](TransmitModel& model) { model.loadProfile(QStringLiteral("TX Profile")); }, {QStringLiteral("profile tx load \"TX Profile\"")}},
        {"loadMicProfile", [](TransmitModel& model) { model.loadMicProfile(QStringLiteral("Mic Profile")); }, {QStringLiteral("profile mic load \"Mic Profile\"")}},
        {"setApdEnabled", [](TransmitModel& model) { model.setApdEnabled(true); }, {QStringLiteral("apd enable=1")}},
        {"setApdSamplerPort", [](TransmitModel& model) { model.setApdSamplerPort(QStringLiteral("ANT1"), QStringLiteral("RX_A")); }, {QStringLiteral("apd sampler tx_ant=ANT1 sample_port=RX_A")}},
        {"resetApdEqualizer", [](TransmitModel& model) { model.resetApdEqualizer(); }, {QStringLiteral("apd reset")}},
        {"setAtuMemories", [](TransmitModel& model) { model.setAtuMemories(true); }, {QStringLiteral("atu set memories_enabled=1")}},
        {"atuClearMemories", [](TransmitModel& model) { model.atuClearMemories(); }, {QStringLiteral("atu clear")}},
    };
    FlexBackend backend;
    QStringList commands;
    backend.setCommandSink([&](const QString& command) { commands.append(command); });
    for (const TxCase& test : cases) {
        TransmitModel model;
        QSignalSpy raw(&model, &TransmitModel::commandReady);
        QObject::connect(&model, &TransmitModel::controlRequested, &backend,
                         [&](const TransmitControlRequest& request) {
            check(backend.requestTransmitControl(request) == ReceiveDispatch::Dispatched,
                  "Flex accepts a supported typed setting");
        });
        commands.clear();
        test.apply(model);
        check(commands == test.expected, test.name);
        check(raw.isEmpty(), "model emits no raw TX wire text");
    }
    TransmitModel model;
    QObject::connect(&model, &TransmitModel::controlRequested, &backend,
                     [&](const TransmitControlRequest& request) { backend.requestTransmitControl(request); });
    model.setCwDelay(300);
    model.setHoldBreakInDelay(true);
    commands.clear();
    model.setCwSpeed(27);
    check(commands == QStringList{"cw wpm 27", "cw break_in_delay 300"},
          "held CW delay follows speed and is not treated as readback");
    commands.clear();
    model.setCwSpeed(27);
    check(commands == QStringList{"cw wpm 27"}, "unchanged speed does not reassert delay");
    commands.clear();
    TransmitDelta delta;
    delta.rfPower = 33;
    model.applyChanges(delta);
    check(commands.isEmpty() && model.rfPowerIsFromRadio(), "radio power readback never becomes a request");
    commands.clear();
    check(backend.requestTransmitControl({TransmitControlRequest::Field::ProcessorEnabled,
              ProcessorSetting{true, 20}}) == ReceiveDispatch::Dispatched
              && commands == QStringList{"transmit set speech_processor_enable=1"},
          "Flex processor enable is independent of the companion level domain");
    commands.clear();
    check(backend.requestTransmitControl({TransmitControlRequest::Field::ProcessorLevel,
              ProcessorSetting{true, 20}}) == ReceiveDispatch::Unsupported && commands.isEmpty(),
          "Flex processor level still rejects values outside its three-step ladder");
}
struct SliceCase {
    const char* name;
    std::function<void(SliceModel&)> apply;
    QStringList expected;
};
void sliceEncoding()
{
    const std::vector<SliceCase> cases{
        {"setTxAntenna", [](SliceModel& model) { model.setTxAntenna(QStringLiteral("ANT2")); }, {QStringLiteral("slice set 7 txant=ANT2")}},
        {"setRit", [](SliceModel& model) { model.setRit(true, 123); }, {QStringLiteral("slice set 7 rit_on=1 rit_freq=123")}},
        {"setXit", [](SliceModel& model) { model.setXit(true, -321); }, {QStringLiteral("slice set 7 xit_on=1 xit_freq=-321")}},
        {"setDaxChannel", [](SliceModel& model) { model.setDaxChannel(3); }, {QStringLiteral("slice set 7 dax=3")}},
        {"setRttyMark", [](SliceModel& model) { model.setRttyMark(2100); }, {QStringLiteral("slice set 7 rtty_mark=2100")}},
        {"setRttyShift", [](SliceModel& model) { model.setRttyShift(250); }, {QStringLiteral("slice set 7 rtty_shift=250")}},
        {"setDiglOffset", [](SliceModel& model) { model.setDiglOffset(100); }, {QStringLiteral("slice set 7 digl_offset=100")}},
        {"setDiguOffset", [](SliceModel& model) { model.setDiguOffset(101); }, {QStringLiteral("slice set 7 digu_offset=101")}},
        {"setTxSlice", [](SliceModel& model) { model.setTxSlice(true); }, {QStringLiteral("slice set 7 tx=1")}},
        {"setActive", [](SliceModel& model) { model.setActive(true); }, {QStringLiteral("slice set 7 active=1")}},
        {"setRecordOn", [](SliceModel& model) { model.setRecordOn(true); }, {QStringLiteral("slice set 7 record=1")}},
        {"setPlayOn", [](SliceModel& model) { model.setPlayOn(true); }, {QStringLiteral("slice set 7 play=1")}},
        {"setFmToneMode", [](SliceModel& model) { model.setFmToneMode(QStringLiteral("ctcss_tx")); }, {QStringLiteral("slice set 7 fm_tone_mode=ctcss_tx")}},
        {"setFmToneValue", [](SliceModel& model) { model.setFmToneValue(QStringLiteral("88.5")); }, {QStringLiteral("slice set 7 fm_tone_value=88.5")}},
        {"setRepeaterOffsetDir", [](SliceModel& model) { model.setRepeaterOffsetDir(QStringLiteral("up")); }, {QStringLiteral("slice set 7 repeater_offset_dir=up")}},
        {"setFmRepeaterOffsetFreq", [](SliceModel& model) { model.setFmRepeaterOffsetFreq(0.6); }, {QStringLiteral("slice set 7 fm_repeater_offset_freq=0.600000")}},
        {"setTxOffsetFreq", [](SliceModel& model) { model.setTxOffsetFreq(-0.6); }, {QStringLiteral("slice set 7 tx_offset_freq=-0.600000")}},
        {"setFmDeviation", [](SliceModel& model) { model.setFmDeviation(3000); }, {QStringLiteral("slice set 7 fm_deviation=3000")}},
        {"setRfGain", [](SliceModel& model) { model.setRfGain(12); }, {QStringLiteral("slice set 7 rfgain=12")}},
        {"setDiversity", [](SliceModel& model) { model.setDiversity(true); }, {QStringLiteral("slice set 7 diversity=1")}},
        {"setEscEnabled", [](SliceModel& model) { model.setEscEnabled(true); }, {QStringLiteral("slice set 7 esc=on")}},
        {"setEscGain", [](SliceModel& model) { model.setEscGain(0.5f); }, {QStringLiteral("slice set 7 esc_gain=0.500000")}},
        {"setEscPhaseShift", [](SliceModel& model) { model.setEscPhaseShift(45); }, {QStringLiteral("slice set 7 esc_phase_shift=45.000000")}},
    };
    FlexBackend backend;
    QStringList commands;
    backend.setSliceCommandSink([&](const QString& command) { commands.append(command); });
    for (const SliceCase& test : cases) {
        SliceModel model(7);
        QSignalSpy raw(&model, &SliceModel::commandReady);
        QObject::connect(&model, &SliceModel::controlRequested, &backend,
                         [&](const SliceControlRequest& request) {
            check(backend.requestSliceControl(7, request) == ReceiveDispatch::Dispatched,
                  "Flex accepts a supported slice setting");
        });
        commands.clear();
        test.apply(model);
        check(commands == test.expected, test.name);
        check(raw.isEmpty(), "model emits no raw slice wire text");
    }
    commands.clear();
    backend.setCommandSink([&](const QString& command) { commands.append(command); });
    check(backend.requestSliceControl(7, {SliceControlRequest::Field::TxAntenna,
              QStringLiteral("ANT1\ntransmit mox 1")}) == ReceiveDispatch::Unsupported,
          "antenna values cannot introduce a second command");
    check(backend.requestSliceControl(7, {SliceControlRequest::Field::EscGain,
              std::numeric_limits<double>::quiet_NaN()}) == ReceiveDispatch::Unsupported,
          "nonfinite slice settings fail closed");
    check(backend.requestTransmitControl({TransmitControlRequest::Field::TxProfile,
              QStringLiteral("name\"\ntransmit mox 1")}) == ReceiveDispatch::Unsupported,
          "profile value cannot escape its quoting");
    check(commands.isEmpty(), "invalid requests produce no output");
}

class ConnectedBackend final : public FlexBackend {
public:
    bool connected{true};
    bool isConnected() const override { return connected; }
};
void routingAndReentrancy()
{
    RadioModel radio;
    auto owned = std::make_unique<ConnectedBackend>();
    ConnectedBackend* backend = owned.get();
    QStringList commands;
    backend->setCommandSink([&](const QString& cmd) { commands.append(cmd); });
    backend->setSliceCommandSink([&](const QString& cmd) { commands.append(cmd); });
    radio.setBackendForTest(std::move(owned), QStringLiteral("model-control-test"));
    SliceDelta delta;
    delta.inUse = true; delta.frequency = 14.2; delta.mode = QStringLiteral("USB");
    delta.panId = QStringLiteral("receiver"); delta.filterLow = 100; delta.filterHigh = 2800;
    emit backend->sliceChanged(0, delta);
    SliceModel* slice = radio.slice(0);
    check(slice != nullptr, "production status creates the routed slice");
    if (!slice) { return; }
    commands.clear();
    slice->setRit(true, 100);
    check(commands == QStringList{"slice set 0 rit_on=1 rit_freq=100"}, "RIT has one route");
    commands.clear();
    QSignalSpy dropped(&radio, &RadioModel::commandDropped);
    slice->applyRecalledFmRepeater(QStringLiteral("up"), 0.6, QStringLiteral("ctcss_tx"), 88.5);
    check(commands.isEmpty() && dropped.isEmpty()
              && slice->repeaterOffsetDir() == QStringLiteral("up"),
          "Flex local-bank recall stays local without an unsupported-control notice");
    backend->connected = false;
    slice->setXit(true, 200);
    check(commands.isEmpty(), "disconnected slice cannot dispatch");
    backend->connected = true;
    commands.clear();
    std::thread wrongThread([slice] { slice->setDaxChannel(4); });
    wrongThread.join();
    check(commands.isEmpty(), "off-thread setter never dispatches");
    TransmitModel* tx = &radio.transmitModel();
    QObject::connect(tx, &TransmitModel::rfPowerChanged, tx, [tx](int power) {
        if (power == 41) { tx->setRfPower(42); }
    });
    tx->setRfPower(41);
    check(commands == QStringList{"transmit set rfpower=42"}, "superseded power intent never dispatches");
    commands.clear();
    QObject::connect(tx, &TransmitModel::micStateChanged, tx, [tx] { tx->invalidateControlIntents(); });
    tx->setMicLevel(32);
    check(commands.isEmpty(), "retired TX setting cannot dispatch after notification");
}

void noOpReentrancy()
{
    SliceModel slice(7);
    QSignalSpy toneChanged(&slice, &SliceModel::fmToneValueChanged);
    QSignalSpy offsetChanged(&slice, &SliceModel::fmRepeaterOffsetFreqChanged);
    QSignalSpy daxChanged(&slice, &SliceModel::daxChannelChanged);
    QSignalSpy escChanged(&slice, &SliceModel::escGainChanged);
    QObject::connect(&slice, &SliceModel::controlRequested, &slice,
                     [&](const SliceControlRequest& request) {
        using Field = SliceControlRequest::Field;
        switch (request.field) {
        case Field::FmToneValue: slice.setFmToneValue(QStringLiteral("88.50")); break;
        case Field::RepeaterOffset: slice.setFmRepeaterOffsetFreq(0.6); break;
        case Field::DaxChannel: slice.setDaxChannel(99); break;
        case Field::EscGain: slice.setEscGain(3.0f); break;
        default: break;
        }
    });
    slice.setFmToneValue(QStringLiteral("88.50"));
    slice.setFmRepeaterOffsetFreq(0.6);
    slice.setDaxChannel(8);
    slice.setEscGain(2.0f);
    check(toneChanged.size() == 1 && offsetChanged.size() == 1
              && daxChanged.size() == 1 && escChanged.size() == 1,
          "reentrant equal or clamped no-ops do not cancel the accepted intent's notification");
}

void heldDelayReentrancy()
{
    TransmitModel model;
    FlexBackend backend;
    QStringList commands;
    QObject::connect(&model, &TransmitModel::controlRequested, &backend,
                     [&](const TransmitControlRequest& request) { backend.requestTransmitControl(request); });
    backend.setCommandSink([&](const QString& command) { commands.append(command); });
    model.setHoldBreakInDelay(true);
    bool nested = false;
    QObject::connect(&model, &TransmitModel::phoneStateChanged, &model, [&] {
        if (!nested) {
            nested = true;
            model.setCwSpeed(27);
        }
    });
    model.setCwDelay(300);
    check(commands == QStringList{"cw wpm 27", "cw break_in_delay 300"},
          "held-delay reassertion retires the outer pending delay intent");
}

void deletionAndMalformedInput()
{
    check(!SliceControlRequest{}.valid() && !TransmitControlRequest{}.valid(),
          "default-constructed control requests fail closed");
    using Field = SliceControlRequest::Field;
    using Origin = SliceControlRequest::Origin;
    check(SliceControlRequest{Field::RepeaterRecall, RepeaterSetting{"up", 600000, "ctcss_tx", 88.5},
                              Origin::RadioDefaultRestore}.valid()
              && !SliceControlRequest{Field::TxSlice, true, Origin::RadioDefaultRestore}.valid()
              && !SliceControlRequest{Field::RepeaterRecall, true, Origin::RadioDefaultRestore}.valid(),
          "restore origin permits valid grouped recall but neither TX selection nor malformed payloads");
    int requests = 0;
    auto tx = std::make_unique<TransmitModel>();
    QObject::connect(tx.get(), &TransmitModel::controlRequested, QCoreApplication::instance(),
                     [&](const TransmitControlRequest&) { ++requests; });
    QObject::connect(tx.get(), &TransmitModel::micStateChanged, QCoreApplication::instance(),
                     [&] { tx.reset(); });
    tx->setMicLevel(17);
    check(!tx && requests == 0, "deletion from a local notification retires the pending TX intent");
    auto slice = std::make_unique<SliceModel>(7);
    QObject::connect(slice.get(), &SliceModel::controlRequested, QCoreApplication::instance(),
                     [&](const SliceControlRequest&) { ++requests; slice.reset(); });
    slice->setFmToneValue(QStringLiteral("not a frequency"));
    check(slice && requests == 0, "invalid tone text is not converted to a zero-valued command");
    slice->setDaxChannel(2);
    check(!slice && requests == 1, "deletion from slice dispatch has no stale notification tail");
}
}
int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("model-control-routing"));
    if (!settings.isValid()) {
        std::fprintf(stderr, "Cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    transmitEncoding();
    sliceEncoding();
    routingAndReentrancy();
    noOpReentrancy();
    heldDelayReentrancy();
    deletionAndMalformedInput();
    return failures ? 1 : 0;
}
