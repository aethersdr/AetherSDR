// Socket-free production routing: dispatch receipts, not capability guesses,
// determine notices. Keying is recorded by the injected backend only.
#include "TestSettingsProfile.h"
#include "IcomReceiveContractTestAccess.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include <QCoreApplication>
#include <QStringList>
#include <cstdio>
#include <memory>
#include <vector>

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}
class Backend : public IRadioBackend {
public:
    bool connected{true};
    RadioCapabilities caps;
    bool handled{false};
    ReceiveDispatch result{ReceiveDispatch::Unsupported};
    std::vector<TransmitControlRequest> requests;
    std::vector<TunePowerContext> contexts;
    QList<QPair<bool, int>> tunes;
    QList<int> cwPitches;
    QList<int> lineoutGains;
    QList<bool> lineoutMutes;
    int keyCalls{0};
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override { ++keyCalls; }
    void setTune(bool on, int power, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override
    { tunes.append({on, power}); }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    void setCwPitch(int hz) override { cwPitches.append(hz); }
    void setLineoutGain(int percent) override { lineoutGains.append(percent); }
    void setLineoutMute(bool mute) override { lineoutMutes.append(mute); }
    ReceiveDispatch requestTransmitControl(const TransmitControlRequest& request, TunePowerContext context) override
    {
        requests.push_back(request);
        contexts.push_back(context);
        return handled ? result : IRadioBackend::requestTransmitControl(request, context);
    }
};
struct Fixture {
    RadioModel radio;
    Backend* backend;
    QStringList dropped;
    Fixture()
    {
        auto owned = std::make_unique<Backend>();
        backend = owned.get();
        backend->caps.canTransmit = true;
        backend->caps.transmitDriveControl = RadioCapabilities::TransmitDriveControl{
            SliceFrequencyControl::Authority::Engine, true};
        radio.setBackendForTest(std::move(owned), QStringLiteral("control-receipt-test"));
        QObject::connect(&radio, &RadioModel::commandDropped, &radio,
                         [this](const QString& operation) { dropped.append(operation); });
        SliceDelta delta;
        delta.inUse = true; delta.frequency = 14.2; delta.mode = QStringLiteral("USB");
        delta.filterLow = 100; delta.filterHigh = 2800;
        delta.panId = QStringLiteral("receiver"); delta.txSlice = true;
        emit backend->sliceChanged(0, delta);
    }
};
void lineoutRouting()
{
    Fixture f;
    f.backend->caps.lineoutControl = RadioCapabilities::LineoutControl{};
    f.backend->caps.radioSetupControl = RadioSetupControl{
        {RadioSetupField::LineoutGain, RadioSetupField::LineoutMute}};
    f.radio.setLineoutMute(true);
    f.radio.setLineoutMute(false);
    f.radio.setLineoutGain(30);
    check(f.backend->lineoutMutes == QList<bool>({true, false})
              && f.backend->lineoutGains == QList<int>{30} && f.dropped.isEmpty(),
          "declared line output receives each typed edit without a drop notice");

    Fixture unsupported;
    unsupported.backend->caps.radioSetupControl = RadioSetupControl{
        {RadioSetupField::LineoutGain, RadioSetupField::LineoutMute}};
    unsupported.radio.setLineoutMute(true);
    unsupported.radio.setLineoutGain(30);
    check(unsupported.backend->lineoutMutes.isEmpty()
              && unsupported.backend->lineoutGains.isEmpty()
              && unsupported.dropped.contains(QStringLiteral("line output mute 1"))
              && unsupported.dropped.contains(QStringLiteral("line output gain 30")),
          "absent line output receives no seam call and retains both drop notices");
}
void setupRefusalLabels()
{
    using Field = RadioSetupField;
    const struct {
        RadioSetupRequest request;
        const char* label;
    } cases[] = {
        {{Field::LineoutGain, 0}, "line output gain 0"},
        {{Field::LineoutGain, 100}, "line output gain 100"},
        {{Field::LineoutMute, false}, "line output mute 0"},
        {{Field::LineoutMute, true}, "line output mute 1"},
        {{Field::HeadphoneGain, 0}, "headphone gain 0"},
        {{Field::HeadphoneGain, 100}, "headphone gain 100"},
        {{Field::HeadphoneMute, false}, "headphone mute 0"},
        {{Field::HeadphoneMute, true}, "headphone mute 1"},
        {{Field::FrontSpeakerMute, false}, "front speaker mute 0"},
        {{Field::FrontSpeakerMute, true}, "front speaker mute 1"},
        {{Field::ShowTxInWaterfall, false}, "show TX in waterfall 0"},
        {{Field::ShowTxInWaterfall, true}, "show TX in waterfall 1"},
        {{Field::MeterDuringReceive, true}, "radio setup"},
    };
    Fixture f;
    for (const auto& item : cases) {
        f.dropped.clear();
        check(f.radio.requestRadioSetup(item.request) == ReceiveDispatch::Unsupported
                  && f.dropped == QStringList{QString::fromLatin1(item.label)}, item.label);
    }
    f.dropped.clear();
    check(f.radio.requestRadioSetup({Field::LineoutGain, true}) == ReceiveDispatch::Unsupported
              && f.radio.requestRadioSetup({Field::HeadphoneMute, 1}) == ReceiveDispatch::Unsupported
              && f.radio.requestRadioSetup({Field::LineoutGain, 101}) == ReceiveDispatch::Unsupported
              && f.dropped.isEmpty(),
          "invalid setup values refuse without formatting or announcing an invalid variant");
}
void receipts()
{
    Fixture f;
    f.radio.slice(0)->setActive(true);
    check(f.radio.slice(0)->isActive() && f.dropped.isEmpty(),
          "local slice selection never consumes the unsupported-control notice");
    f.radio.slice(0)->applyRecalledFmRepeater(QStringLiteral("up"), 0.6,
                                           QStringLiteral("ctcss_tx"), 88.5);
    check(f.dropped.isEmpty() && f.radio.slice(0)->fmToneMode() == QStringLiteral("ctcss_tx"),
          "local memory recall applies its state without consuming the refusal notice");
    TransmitModel& tx = f.radio.transmitModel();
    tx.setRfPower(42);
    check(f.backend->requests.size() == 1 && f.dropped.size() == 1,
          "advertised capability with inherited no-op still refuses");
    f.backend->handled = true;
    f.backend->result = ReceiveDispatch::Dispatched;
    f.dropped.clear();
    f.backend->requests.clear();
    tx.setMicLevel(34);
    tx.setTxFilter(200, 2800);
    tx.setVoxEnable(true);
    tx.setVoxLevel(35);
    tx.setSbMonitor(true);
    tx.setSpeechProcessorLevel(1);
    check(f.backend->requests.size() == 6 && f.dropped.isEmpty(),
          "six explicitly dispatched settings each have one route and no drop");
    const auto& voxRequest = f.backend->requests[3];
    const VoxSetting vox = std::get<VoxSetting>(voxRequest.value);
    check(voxRequest.field == TransmitControlRequest::Field::VoxLevel
              && vox.enabled && vox.level == 35,
          "VOX request retains the edited field and paired state");
    check(f.backend->keyCalls == 0 && f.backend->tunes.isEmpty(),
          "settings never synthesize keying or TUNE");
    f.backend->result = ReceiveDispatch::LocalOnly;
    tx.setSpeechProcessorEnable(true);
    check(f.dropped.isEmpty(), "real local application is not an unsupported control");
    f.backend->result = ReceiveDispatch::Unsupported;
    tx.setCwDelay(300);
    check(f.dropped.size() == 1, "explicit refusal is surfaced");
    const size_t count = f.backend->requests.size();
    TransmitDelta status; status.rfPower = 55; status.micLevel = 56;
    tx.applyChanges(status);
    check(f.backend->requests.size() == count, "readback is observation only");
    f.backend->connected = false;
    tx.setMicLevel(57);
    check(f.backend->requests.size() == count, "disconnected settings cannot reach backend");
}
void tuneOwnership()
{
    Fixture f;
    f.backend->handled = true;
    f.backend->result = ReceiveDispatch::Dispatched;
    TransmitModel& tx = f.radio.transmitModel();
    tx.setTunePower(25);
    check(f.backend->contexts.back() == TunePowerContext::Deferred,
          "idle TUNE power is a deferred setpoint");
    tx.startTune();
    check(!f.backend->tunes.isEmpty() && f.backend->tunes.first() == qMakePair(true, 25),
          "existing TX gate starts TUNE with the requested setpoint");
    tx.setTunePower(30);
    check(f.backend->contexts.back() == TunePowerContext::LiveLocalCarrier,
          "only admitted live TUNE supplies the local-carrier context");
    const auto keyed = f.backend->tunes;
    TransmitDelta echo; echo.tunePower = 45;
    const size_t requests = f.backend->requests.size();
    tx.applyChanges(echo);
    check(f.backend->requests.size() == requests && f.backend->tunes == keyed,
          "power readback neither reapplies power nor rekeys");
    tx.stopTune();
    tx.setTunePower(50);
    check(f.backend->contexts.back() == TunePowerContext::Deferred,
          "release retires live-carrier eligibility");
    Fixture other;
    TransmitDelta remote; remote.tune = true;
    other.radio.transmitModel().applyChanges(remote);
    other.radio.transmitModel().setTunePower(30);
    check(other.backend->contexts.back() == TunePowerContext::UnqualifiedCarrier,
          "radio-reported TUNE is not local ownership");
    check(other.backend->tunes.isEmpty(), "unqualified carrier is never keyed locally");
}

void filterWarningReentrancy()
{
    Fixture f;
    f.backend->handled = true;
    f.backend->result = ReceiveDispatch::Dispatched;
    SliceDelta status; status.frequency = 14.349;
    f.radio.slice(0)->applyChanges(status);
    bool corrected = false;
    QObject::connect(&f.radio, &RadioModel::interlockNotificationRequested, &f.radio, [&] {
        if (!corrected) {
            corrected = true;
            f.radio.transmitModel().setTxFilter(100, 800);
        }
    });
    f.radio.transmitModel().setTxFilter(100, 3000);
    check(corrected && f.backend->requests.size() == 1
              && std::get<TxPassband>(f.backend->requests.front().value).highHz == 800,
          "a filter-warning callback cannot dispatch the superseded passband after the correction");
}

void cwPitchBackendLifetime()
{
    Fixture f;
    f.backend->caps.hostModulates = true;
    TransmitModel& tx = f.radio.transmitModel();
    tx.setCwPitch(700);
    tx.setCwPitch(700);
    check(f.backend->cwPitches == QList<int>{700} && f.backend->requests.empty()
              && f.dropped.isEmpty(),
          "host CW pitch is applied once, without duplicate dispatch or a false drop");
    TransmitDelta status;
    status.cwPitch = 800;
    tx.applyChanges(status);
    check(f.backend->cwPitches == QList<int>({700, 800}) && f.backend->requests.empty(),
          "radio CW pitch readback updates the host BFO without inventing operator intent");

    auto fresh = std::make_unique<Backend>();
    Backend* second = fresh.get();
    second->caps = f.backend->caps;
    second->handled = true;
    second->result = ReceiveDispatch::Dispatched;
    f.radio.setBackendForTest(std::move(fresh), QStringLiteral("control-receipt-test"));
    f.backend = second;
    tx.setCwPitch(800);
    check(second->requests.size() == 1
              && second->requests.front().field == TransmitControlRequest::Field::CwPitch
              && std::get<int>(second->requests.front().value) == 800
              && f.dropped.isEmpty(),
          "a pitch handed only to the old backend is dispatched to its replacement");
}

void icomMicReadbackWindow()
{
    using Access = icom::IcomCivBackendTestAccess;
    RadioModel radio;
    auto owned = std::make_unique<icom::IcomCivBackend>();
    icom::IcomCivBackend* backend = owned.get();
    radio.setBackendForTest(std::move(owned), QStringLiteral("icom-mic-readback-test"));
    Access::prepare(*backend);
    Access::selectModel(*backend, *icom::modelForId(0xA2));
    QStringList dropped;
    QObject::connect(&radio, &RadioModel::commandDropped, &radio,
                     [&](const QString& operation) { dropped.append(operation); });
    // IC-9700 CI-V guide: DATA OFF MOD input is SET 0115; LAN is 05.
    icom::CivFrame input;
    input.cmd = icom::cmd::kSetting;
    input.hasSub = true;
    input.sub = 0x05;
    input.data = {0x01, 0x15, 0x05};
    Access::observe(*backend, input);
    const auto queued = Access::queuedCount(*backend);
    const auto dispatched = Access::dispatchCount(*backend);
    radio.transmitModel().setMicLevel(34);
    check(dropped.isEmpty() && Access::queuedCount(*backend) == queued
              && Access::dispatchCount(*backend) == dispatched,
          "unestablished LAN mic readback neither writes a register nor reports unsupported");
    // The radio establishes LAN MOD level via SET 0114 before an operator edit.
    icom::CivFrame level = input;
    level.data = {0x01, 0x14, 0x00, 0x26};
    Access::observe(*backend, level);
    check(Access::queuedCount(*backend) == queued && Access::dispatchCount(*backend) == dispatched,
          "LAN level readback does not replay the earlier slider intent");
    radio.transmitModel().setMicLevel(35);
    Access::pump(*backend);
    check(dropped.isEmpty()
              && Access::dispatched(*backend, QStringLiteral("fe fe a4 e0 1a 05 01 14 00 90 fd")),
          "after LAN readback, the routed slider dispatches the actual LAN MOD write");
}
}
void txSliceSelectionNotice()
{
    Fixture receiveOnly;
    receiveOnly.backend->caps.canTransmit = false;
    SliceModel* slice = receiveOnly.radio.slice(0);
    check(slice != nullptr, "receive-only fixture has a routed slice");
    if (!slice) { return; }
    slice->setTxSlice(true);
    check(receiveOnly.dropped.isEmpty(),
          "choosing a TX slice on a radio that cannot transmit spends no notice");
    Fixture transmitting;
    if (SliceModel* txSlice = transmitting.radio.slice(0)) { txSlice->setTxSlice(true); }
    check(transmitting.dropped == QStringList{QStringLiteral("tx slice")},
          "a transmit-capable radio that refuses TX-slice selection still reports it");
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("transmit-control-receipts"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    receipts();
    lineoutRouting();
    setupRefusalLabels();
    tuneOwnership();
    filterWarningReentrancy();
    cwPitchBackendLifetime();
    icomMicReadbackWindow();
    txSliceSelectionNotice();
    return failures ? 1 : 0;
}
