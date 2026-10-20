// FlexLib 4.2.18 Radio.cs, Xvtr.cs, TxBandSettings.cs and Usb*Cable.cs.
// Wire and independently injected observations; no sockets or RF.
#include "TestSettingsProfile.h"
#include "RadioSetupControlFixture.h"
#include "core/AppSettings.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/icom/IcomCivBackend.h"
#include "core/backends/sim/SimBackend.h"
#include "core/backends/anan/AnanBackend.h"
#include "core/backends/rtl/RtlSdrBackend.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <limits>
#include <set>
#include <utility>
#include <vector>

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool success, const char* message)
{
    std::printf("[%s] %s\n", success ? "PASS" : "FAIL", message);
    failures += !success;
}
void radioEncoding()
{
    using Field = RadioSetupRequest::Field;
    struct Case { RadioSetupRequest request; QStringList expected; };
    const std::vector<Case> cases{
        {{Field::Callsign, QStringLiteral("KI4TTZ")}, {"radio callsign KI4TTZ"}},
        {{Field::ClientStation, QStringLiteral("Test Client")}, {QStringLiteral("client station Test") + QChar(0x7f) + QStringLiteral("Client")}},
        {{Field::PrivateIpPolicy, true}, {"radio set enforce_private_ip_connections=1"}},
        {{Field::NetworkMtu, 1200}, {"client set enforce_network_mtu=1 network_mtu=1200"}},
        {{Field::Dhcp, std::monostate{}}, {"radio static_net_params reset"}},
        {{Field::StaticNetwork, StaticNetwork{QHostAddress("192.0.2.10"), QHostAddress("192.0.2.1"), QHostAddress("255.255.255.0")}}, {"radio static_net_params ip=192.0.2.10 gateway=192.0.2.1 netmask=255.255.255.0"}},
        {{Field::AccTxDelayMs, 11}, {"interlock acc_tx_delay=11"}},
        {{Field::TxDelayMs, 30}, {"interlock tx_delay=30"}},
        {{Field::Tx1DelayMs, 12}, {"interlock tx1_delay=12"}},
        {{Field::Tx2DelayMs, 13}, {"interlock tx2_delay=13"}},
        {{Field::Tx3DelayMs, 14}, {"interlock tx3_delay=14"}},
        {{Field::TimeoutMs, 45000}, {"interlock timeout=45000"}},
        {{Field::MaximumPowerPercent, 80}, {"transmit set max_power_level=80"}},
        {{Field::ShowTxInWaterfall, true}, {"transmit set show_tx_in_waterfall=1"}},
        {{Field::MeterDuringReceive, true}, {"transmit set met_in_rx=1"}},
        {{Field::SyncCwx, true}, {"cw synccwx 1"}},
        {{Field::RttyMarkDefaultHz, 2125}, {"radio set rtty_mark_default=2125"}},
        {{Field::CalibrationFrequencyMhz, 10.0}, {"radio set cal_freq=10.000000"}},
        {{Field::FrequencyCorrectionPpb, -17}, {"radio set freq_error_ppb=-17"}},
        {{Field::ReferenceSource, ReferenceSource::External}, {"radio oscillator external"}},
        {{Field::MuteLocalWhenRemote, true}, {"radio set mute_local_audio_when_remote=1"}},
        {{Field::BinauralReceive, true}, {"radio set binaural_rx=1"}},
        {{Field::SharpnessLevel, SharpnessLevel{SharpnessMode::Cw, 3}}, {"radio filter_sharpness cw level=3"}},
        {{Field::SharpnessAuto, SharpnessAuto{SharpnessMode::Voice, false}}, {"radio filter_sharpness voice auto_level=0"}},
        {{Field::LowLatencyDigital, true}, {"radio set low_latency_digital_modes=1"}},
        {{Field::LineoutGain, 42}, {"mixer lineout gain 42"}},
        {{Field::LineoutMute, true}, {"mixer lineout mute 1"}},
        {{Field::HeadphoneGain, 43}, {"mixer headphone gain 43"}},
        {{Field::HeadphoneMute, false}, {"mixer headphone mute 0"}},
        {{Field::FrontSpeakerMute, true}, {"mixer front_speaker mute 1"}},
        {{Field::StartFrequencyCalibration, 10.0}, {"radio set cal_freq=10.000000", "radio set freq_error_ppb=0", "radio pll_start"}},
    };
    RadioSetupTestBackend backend;
    std::set<Field> covered;
    for (const Case& item : cases) {
        backend.commands.clear();
        backend.replies.clear();
        const ReceiveDispatch result = backend.requestRadioSetup(item.request);
        if (item.request.field == Field::StartFrequencyCalibration) { backend.acceptCalibrationPrelude(); }
        check(result == ReceiveDispatch::Dispatched
            && backend.commands == item.expected, "radio setup dispatch has exact wire output");
        covered.insert(item.request.field);
    }
    check(covered.size() == static_cast<size_t>(Field::Count), "every radio setup field has a positive encoder check");
    for (const ReferenceSource source : {ReferenceSource::Auto, ReferenceSource::Gpsdo, ReferenceSource::Tcxo}) {
        backend.commands.clear();
        const QString text = source == ReferenceSource::Auto ? "auto" : source == ReferenceSource::Gpsdo ? "gpsdo" : "tcxo";
        check(backend.requestRadioSetup({Field::ReferenceSource, source}) == ReceiveDispatch::Dispatched
            && backend.commands == QStringList{QStringLiteral("radio oscillator ") + text}, "every reference selection has an encoder");
    }
    for (const SharpnessMode mode : {SharpnessMode::Voice, SharpnessMode::Cw, SharpnessMode::Digital}) {
        const QString text = mode == SharpnessMode::Voice ? "voice" : mode == SharpnessMode::Cw ? "cw" : "digital";
        backend.commands.clear();
        backend.requestRadioSetup({Field::SharpnessLevel, SharpnessLevel{mode, 2}});
        backend.requestRadioSetup({Field::SharpnessAuto, SharpnessAuto{mode, true}});
        check(backend.commands == QStringList{QStringLiteral("radio filter_sharpness %1 level=2").arg(text),
            QStringLiteral("radio filter_sharpness %1 auto_level=1").arg(text)}, "every sharpness mode preserves separate level and auto fields");
    }
    backend.commands.clear();
    backend.requestRadioSetup({Field::Callsign, QStringLiteral("ki4ttz /p_#")});
    backend.requestRadioSetup({Field::ClientStation, QStringLiteral("Desk @#._-/ ]")});
    check(backend.commands == QStringList{"radio callsign KI4TTZ/P",
        QStringLiteral("client station Desk") + QChar(0x7f) + QStringLiteral("_-/") + QChar(0x7f) + QStringLiteral("]")},
        "callsign allowlist and station denylist match distinct SDK rules");
    backend.commands.clear();
    const std::vector<RadioSetupRequest> invalid{
        {Field::FrequencyCorrectionPpb, true}, {Field::NetworkMtu, 0},
        {Field::TimeoutMs, -1}, {Field::MaximumPowerPercent, 101},
        {Field::ReferenceSource, static_cast<ReferenceSource>(90)},
        {Field::CalibrationFrequencyMhz, std::numeric_limits<double>::infinity()},
        {Field::StartFrequencyCalibration, -10.0},
        {Field::SharpnessLevel, SharpnessLevel{SharpnessMode::Voice, 4}},
        {Field::StaticNetwork, StaticNetwork{QHostAddress("192.0.2.1"), QHostAddress("::1"), QHostAddress("255.255.255.0")}},
        {Field::StaticNetwork, StaticNetwork{QHostAddress("192.0.2.1"), QHostAddress("192.0.2.2"), QHostAddress("255.0.255.0")}},
        {Field::Count, 1}, {static_cast<Field>(999), false},
    };
    for (const RadioSetupRequest& request : invalid) {
        check(backend.requestRadioSetup(request) == ReceiveDispatch::Unsupported, "malformed radio setting is refused");
    }
    check(backend.commands.isEmpty(), "invalid settings produce no wire effects");
    backend.accept = false;
    check(backend.requestRadioSetup({Field::StartFrequencyCalibration, 10.0}) == ReceiveDispatch::Unsupported
        && backend.commands.isEmpty(), "an undispatched prelude never starts calibration");
}

void bandAndTransverterEncoding()
{
    using Field = TxBandRequest::Field;
    RadioSetupTestBackend backend;
    const QStringList bandCommands{
        "transmit bandset 9 rfpower=35", "transmit bandset 9 tunepower=35",
        "transmit bandset 9 inhibit=1", "interlock bandset 9 acc_tx_enabled=1",
        "interlock bandset 9 rca_txreq_enable=1", "interlock bandset 9 acc_txreq_enable=1",
        "interlock bandset 9 tx1_enabled=1", "interlock bandset 9 tx2_enabled=1",
        "interlock bandset 9 tx3_enabled=1", "transmit bandset 9 hwalc_enabled=1"};
    for (int field = 0; field < int(Field::Count); ++field) {
        const Field selected = static_cast<Field>(field);
        const std::variant<int, bool> value = field < 2 ? std::variant<int, bool>(35) : std::variant<int, bool>(true);
        backend.commands.clear();
        check(backend.requestTxBand({9, selected, value}) == ReceiveDispatch::Dispatched
            && backend.commands == QStringList{bandCommands[field]}, "every band field reaches its correct transmit/interlock register");
        if (field >= 2) {
            backend.commands.clear();
            QString disabled = bandCommands[field];
            disabled[disabled.size() - 1] = QLatin1Char('0');
            check(backend.requestTxBand({9, selected, false}) == ReceiveDispatch::Dispatched
                && backend.commands == QStringList{disabled}, "every band disable retains zero polarity");
        }
    }
    using X = TransverterRequest::Field;
    const std::vector<std::pair<TransverterRequest, QString>> cases{
        {{-1, X::Create, std::monostate{}}, "xvtr create"},
        {{2, X::Remove, std::monostate{}}, "xvtr remove 2"},
        {{2, X::Name, QStringLiteral("1296MHz")}, "xvtr set 2 name=1296"},
        {{2, X::RfFrequencyMhz, 144.0}, "xvtr set 2 rf_freq=144.000000"},
        {{2, X::IfFrequencyMhz, 28.0}, "xvtr set 2 if_freq=28.000000"},
        {{2, X::LoErrorMhz, -0.000123}, "xvtr set 2 lo_error=-0.000123"},
        {{2, X::RxGainDb, -3.5}, "xvtr set 2 rx_gain=-3.50"},
        {{2, X::MaximumPowerDbm, 99.0, 144.0}, "xvtr set 2 max_power=8.00"},
        {{2, X::RxOnly, true}, "xvtr set 2 rx_only=1"},
    };
    for (const auto& [request, expected] : cases) {
        backend.commands.clear();
        check(backend.requestTransverter(request) == ReceiveDispatch::Dispatched
            && backend.commands == QStringList{expected}, "each transverter operation has exact wire output");
    }
    backend.setModelProvider([] { return QStringLiteral("FLEX-6600"); });
    backend.commands.clear();
    backend.requestTransverter({2, X::MaximumPowerDbm, 15.0, 28.0});
    check(backend.commands == QStringList{"xvtr set 2 max_power=10.00"}, "backend enforces the model/IF dependent power ceiling");
    backend.commands.clear();
    check(backend.requestTransverter({2, X::Name, QStringLiteral("a=1 ")}) == ReceiveDispatch::Unsupported
        && backend.requestTransverter({2, X::LoErrorMhz, std::numeric_limits<double>::quiet_NaN()}) == ReceiveDispatch::Unsupported
        && backend.requestTxBand({9, Field::TunePowerPercent, 101}) == ReceiveDispatch::Unsupported
        && backend.commands.isEmpty(), "malformed transverter and band inputs fail closed");
}

void usbEncoding()
{
    using F = UsbCableRequest::Field;
    using Value = decltype(UsbCableRequest::value);
    struct Case { F field; Value value; QString expected; int bit{-1}; };
    const std::vector<Case> cases{
        {F::Type, QStringLiteral("cat"), "type=cat"},
        {F::Name, QStringLiteral("My Cable"), QStringLiteral("name=My") + QChar(0x7f) + QStringLiteral("Cable")},
        {F::Enabled, true, "enable=1"}, {F::AutoReport, false, "auto_report=0"},
        {F::Source, QStringLiteral("tx_slice"), "source=tx_slice"},
        {F::SerialSpeed, 115200, "speed=115200"}, {F::DataBits, 7, "data_bits=7"},
        {F::Parity, QStringLiteral("even"), "parity=even"}, {F::StopBits, 2, "stop_bits=2"},
        {F::FlowControl, QStringLiteral("rts_cts"), "flow_control=rts_cts"},
        {F::ActiveHigh, false, "polarity=active_low"},
        {F::Band, QStringLiteral("4"), "band=4"}, {F::Preamp, true, "preamp=1"},
        {F::SourceRxAntenna, QStringLiteral("RX_A"), "source_rx_ant=RX_A", 2},
        {F::SourceTxAntenna, QStringLiteral("ANT1"), "source_tx_ant=ANT1", 2},
        {F::SourceSlice, QStringLiteral("A"), "source_slice=A", 2},
        {F::Output, QStringLiteral("band"), "output=band", 2},
        {F::LowFrequencyMhz, 14.0, "low_freq=14.000000", 2},
        {F::HighFrequencyMhz, 14.35, "high_freq=14.350000", 2},
        {F::PttDependent, true, "ptt_dependent=1", 2},
        {F::PttDelayMs, 15, "ptt_delay=15", 2}, {F::TxDelayMs, 20, "tx_delay=20", 2},
    };
    RadioSetupTestBackend backend;
    std::set<F> covered;
    for (const Case& item : cases) {
        backend.commands.clear();
        const QString prefix = item.bit < 0 ? "usb_cable set FTDI01 " : "usb_cable setbit FTDI01 2 ";
        check(backend.requestUsbCable({QStringLiteral("FTDI01"), item.bit, item.field, item.value})
            == ReceiveDispatch::Dispatched && backend.commands == QStringList{prefix + item.expected},
            "USB cable field is encoded by the backend, including per-bit routing");
        covered.insert(item.field);
    }
    backend.commands.clear();
    check(backend.requestUsbCable({QStringLiteral("FTDI01"), -1, F::Remove, std::monostate{}})
        == ReceiveDispatch::Dispatched && backend.commands == QStringList{"usb_cable remove FTDI01"},
        "cable removal is a typed operation");
    covered.insert(F::Remove);
    check(covered.size() == size_t(F::Count), "every cable setting has positive encoder coverage");
    backend.commands.clear();
    const std::vector<UsbCableRequest> invalid{
        {QStringLiteral("SN\ntransmit mox 1"), -1, F::Enabled, true},
        {QStringLiteral("SN"), 8, F::Enabled, true},
        {QStringLiteral("SN"), 1, F::Name, QStringLiteral("name")},
        {QStringLiteral("SN"), -1, F::Source, QStringLiteral("tx_slice enable=1")},
        {QStringLiteral("SN"), -1, F::SerialSpeed, 12345},
        {QStringLiteral("SN"), -1, F::DataBits, 9},
        {QStringLiteral("SN"), -1, F::Parity, QStringLiteral("odd enable=1")},
        {QStringLiteral("SN"), 2, F::TxDelayMs, -1},
        {QStringLiteral("SN"), 2, F::LowFrequencyMhz, std::numeric_limits<double>::quiet_NaN()},
    };
    for (const UsbCableRequest& request : invalid) {
        check(backend.requestUsbCable(request) == ReceiveDispatch::Unsupported, "malformed cable input is refused");
    }
    check(backend.commands.isEmpty(), "invalid cable input produces no wire output");
    check(backend.requestUsbCable({QStringLiteral("FTDI01"), 2, F::Band, QString()}) == ReceiveDispatch::Dispatched
        && backend.commands == QStringList{"usb_cable setbit FTDI01 2 band="}, "empty USB bit band retains the SDK clearing operation");
}

void routingAndReplies()
{
    using F = RadioSetupRequest::Field;
    using X = TransverterRequest::Field;
    RadioModel radio;
    RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
    check(radio.isConnected(), "injected transport represents a connected session");
    QSignalSpy dropped(&radio, &RadioModel::commandDropped);
    const int originalPpb = radio.freqErrorPpb();
    check(radio.requestRadioSetup({F::FrequencyCorrectionPpb, 123}) == ReceiveDispatch::Dispatched
        && backend->commands == QStringList{"radio set freq_error_ppb=123"}
        && radio.freqErrorPpb() == originalPpb, "setting acceptance does not fabricate radio readback");
    RadioDelta observation;
    observation.freqErrorPpb = 122;
    backend->commands.clear();
    emit backend->radioChanged(observation);
    check(radio.freqErrorPpb() == 122 && backend->commands.isEmpty(), "independent radio observation wins without echoing a write");
    check(radio.requestTxBand({9, TxBandRequest::Field::RfPowerPercent, 40}) == ReceiveDispatch::Unsupported,
          "unknown band id is refused even with a working encoder");
    RadioSetupControlTestAccess::band(radio, 9);
    backend->commands.clear();
    check(radio.txBandSettings().contains(9)
        && radio.requestTxBand({9, TxBandRequest::Field::RfPowerPercent, 40}) == ReceiveDispatch::Dispatched
        && backend->commands == QStringList{"transmit bandset 9 rfpower=40"}
        && radio.txBandSettings().value(9).rfPower == 37, "known band dispatch waits for authoritative power observation");
    check(radio.requestTransverter({2, X::RxOnly, true}) == ReceiveDispatch::Unsupported,
          "unknown transverter id is refused");
    RadioSetupControlTestAccess::transverter(radio, 2);
    backend->commands.clear();
    check(radio.requestTransverter({2, X::RxOnly, true}) == ReceiveDispatch::Dispatched
        && backend->commands == QStringList{"xvtr set 2 rx_only=1"}
        && !radio.xvtrList().value(2).rxOnly, "transverter intent never invents radio state");
    radio.usbCableModel().applyStatus(QStringLiteral("FTDI01"),
        {{QStringLiteral("type"), QStringLiteral("bit")}, {QStringLiteral("name"), QStringLiteral("Original")}});
    QSignalSpy intents(&radio.usbCableModel(), &UsbCableModel::controlRequested);
    backend->commands.clear();
    radio.usbCableModel().requestSetting(QStringLiteral("FTDI01"), UsbCableRequest::Field::Enabled, QStringLiteral("1"), 3);
    check(backend->commands == QStringList{"usb_cable setbit FTDI01 3 enable=1"}
        && intents.size() == 1
        && radio.usbCableModel().metaObject()->indexOfSignal("commandReady(QString)") == -1,
          "production cable model has one typed route and no raw emission");
    backend->commands.clear();
    check(radio.requestUsbCable({QStringLiteral("unknown"), -1, UsbCableRequest::Field::Enabled, true})
        == ReceiveDispatch::Unsupported && backend->commands.isEmpty(), "unknown cable serial cannot acquire a wire target");
    QObject context;
    int completions = 0;
    RadioSetupReply last{RadioSetupReply::Kind::Refused};
    const RadioSetupCompletion completion = [&](const RadioSetupReply& reply) { ++completions; last = reply; };
    backend->commands.clear();
    radio.requestRadioSetup({F::StartFrequencyCalibration, 10.0}, &context, completion);
    check(backend->commands.size() == 1, "calibration waits for the reference acknowledgement");
    backend->acceptCalibrationPrelude();
    check(backend->commands.size() == 3 && completions == 0, "calibration start remains response-bearing");
    const RadioSetupCompletion accepted = backend->replies.back();
    accepted({RadioSetupReply::Kind::Accepted, 0, QStringLiteral("ok")});
    accepted({RadioSetupReply::Kind::Failed, 7, QStringLiteral("duplicate")});
    check(completions == 1 && last.kind == RadioSetupReply::Kind::Accepted,
          "a reply is delivered exactly once, without confirming calibration complete");
    radio.requestTransverter({-1, X::Create, std::monostate{}}, &context, completion);
    const RadioSetupCompletion failed = backend->replies.back();
    failed({RadioSetupReply::Kind::Failed, 0x50001000, QStringLiteral("rejected")});
    check(completions == 2 && last.code == 0x50001000 && last.kind == RadioSetupReply::Kind::Failed,
          "radio creation failure is retained without retrying or creating local state");
    auto dialog = std::make_unique<QObject>();
    radio.requestTransverter({-1, X::Create, std::monostate{}}, dialog.get(), completion);
    const RadioSetupCompletion closed = backend->replies.back();
    dialog.reset();
    closed({RadioSetupReply::Kind::Accepted});
    check(completions == 2, "closed reply context receives no trailing callback");
    radio.requestTransverter({-1, X::Create, std::monostate{}}, &context, completion);
    const RadioSetupCompletion stale = backend->replies.back();
    const RadioControlSession session = radio.controlSession();
    backend = installRadioSetupBackend(radio);
    stale({RadioSetupReply::Kind::Accepted});
    check(completions == 3 && last.kind == RadioSetupReply::Kind::Cancelled
        && !radio.controlSessionCurrent(session) && backend->commands.isEmpty(),
          "replacement fences an old successful reply and emits no new-session write");
    check(radio.usbCableModel().cables().isEmpty(), "backend replacement retires the old USB inventory");
    backend->live = false;
    RadioSetupControlTestAccess::disconnected(radio);
    backend->commands.clear();
    check(radio.requestRadioSetup({F::TimeoutMs, 45000}) == ReceiveDispatch::Unsupported
        && backend->commands.isEmpty(), "disconnected configuration dispatch is refused");
    check(!dropped.isEmpty(), "unsupported operator settings remain visible refusals");
}

void backendRefusals()
{
    RadioModel radio;
    for (const QString family : {QStringLiteral("sim"), QStringLiteral("hl2"), QStringLiteral("icom"),
                                 QStringLiteral("anan"), QStringLiteral("rtl")}) {
        if (!radio.rebuildBackendForTest(family)) { continue; }
        IRadioBackend* backend = radio.backend();
        check(backend->requestRadioSetup({RadioSetupRequest::Field::StartFrequencyCalibration, 10.0})
            == ReceiveDispatch::Unsupported
            && backend->requestTxBand({9, TxBandRequest::Field::RfPowerPercent, 35}) == ReceiveDispatch::Unsupported
            && backend->requestTransverter({-1, TransverterRequest::Field::Create, std::monostate{}}) == ReceiveDispatch::Unsupported
            && backend->requestUsbCable({QStringLiteral("SN"), -1, UsbCableRequest::Field::Enabled, true}) == ReceiveDispatch::Unsupported,
            "native/RX-only backend does not fabricate support for vendor setup operations");
    }
}

void reentrantCalibrationAndLocalSettings()
{
    RadioModel radio;
    RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
    QStringList writes;
    backend->setSetupCommandSink([&](const QString& command, const RadioSetupCompletion& reply) {
        writes.append(command);
        if (writes.size() == 1) { RadioSetupControlTestAccess::replaceConnectionGeneration(radio); }
        if (reply) { reply({RadioSetupReply::Kind::Accepted}); }
        return true;
    });
    check(radio.requestRadioSetup({RadioSetupRequest::Field::StartFrequencyCalibration, 10.0})
        == ReceiveDispatch::Dispatched && writes == QStringList{"radio set cal_freq=10.000000"},
        "a same-backend reconnect between calibration writes cannot reset or start the next session");
    backend->live = false;
    int replies = 0;
    RadioSetupReply last{RadioSetupReply::Kind::Refused};
    radio.requestRadioSetup({RadioSetupRequest::Field::Callsign, QStringLiteral("N0CALL")}, &radio,
        [&](const RadioSetupReply& result) { ++replies; last = result; });
    check(replies == 1 && last.kind == RadioSetupReply::Kind::Accepted
        && AppSettings::instance().value("StationCallsign").toString() == QLatin1String("N0CALL"),
        "client callsign is genuinely persisted without a radio write while disconnected");
    check(radio.requestRadioSetup({RadioSetupRequest::Field::Callsign, QStringLiteral(" ")})
        == ReceiveDispatch::Unsupported
        && AppSettings::instance().value("StationCallsign").toString() == QLatin1String("N0CALL"),
        "an empty callsign cannot erase the client setting through the typed API");
    radio.requestRadioSetup({RadioSetupRequest::Field::ClientStation, QStringLiteral("Test Client")});
    check(AppSettings::instance().value("StationName").toString() == QLatin1String("Test Client"),
        "station name keeps its client-owned persistence");
    radio.rebuildBackendForTest(QStringLiteral("hl2"));
    radio.requestRadioSetup({RadioSetupRequest::Field::ShowTxInWaterfall, true}, &radio,
        [&](const RadioSetupReply& result) { ++replies; last = result; });
    check(replies == 2 && last.kind == RadioSetupReply::Kind::Accepted
        && radio.transmitModel().showTxInWaterfall(), "offline local display preference has an honest local receipt");
}

void calibrationAcknowledgements()
{
    using Field = RadioSetupRequest::Field;
    for (const int rejectedStep : {0, 1}) {
        RadioModel radio;
        RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
        int completions = 0;
        RadioSetupReply last{RadioSetupReply::Kind::Accepted};
        radio.requestRadioSetup({Field::StartFrequencyCalibration, 10.0}, &radio,
            [&](const RadioSetupReply& reply) { ++completions; last = reply; });
        check(backend->commands == QStringList{"radio set cal_freq=10.000000"}, "start cannot precede a reference reply");
        if (rejectedStep == 1) {
            backend->replyLast({RadioSetupReply::Kind::Accepted});
            check(backend->commands.size() == 2 && backend->commands.last() == "radio set freq_error_ppb=0",
                  "accepted reference starts only the correction reset");
        }
        const RadioSetupCompletion rejected = backend->replies.back();
        rejected({RadioSetupReply::Kind::Failed, 0x50000015, "refused"});
        rejected({RadioSetupReply::Kind::Accepted});
        check(completions == 1 && last.kind == RadioSetupReply::Kind::Failed && last.code == 0x50000015
            && backend->commands.size() == rejectedStep + 1, "a negative prelude reply ends calibration exactly once without PLL start");
    }
    RadioModel radio;
    RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
    auto context = std::make_unique<QObject>();
    radio.requestRadioSetup({Field::StartFrequencyCalibration, 10.0}, context.get(), [](const RadioSetupReply&) {});
    const RadioSetupCompletion abandoned = backend->replies.back();
    context.reset();
    abandoned({RadioSetupReply::Kind::Accepted});
    check(backend->commands.size() == 1, "closing the calibration owner stops every later write");

    backend->commands.clear(); backend->replies.clear();
    int completions = 0;
    radio.requestRadioSetup({Field::StartFrequencyCalibration, 10.0}, &radio, [&](const RadioSetupReply& reply) {
        ++completions;
        check(reply.kind == RadioSetupReply::Kind::Failed && reply.code == -1, "unanswered prelude has a bounded failure");
    });
    const RadioSetupCompletion late = backend->replies.back();
    QEventLoop wait;
    QTimer::singleShot(31000, &wait, &QEventLoop::quit);
    wait.exec();
    late({RadioSetupReply::Kind::Accepted});
    check(completions == 1 && backend->commands.size() == 1, "a timed-out prelude can never reset or start calibration later");
}

void productionSinkAndLocalReceipts()
{
    RadioModel radio;
    RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
    QStringList writes;
    std::vector<RadioConnection::ResponseCallback> responses;
    const auto sink = RadioSetupControlTestAccess::productionSetupSink(radio,
        [&](const QString& command, RadioConnection::ResponseCallback reply) {
            writes.append(command); responses.push_back(reply); return true;
        });
    int completions = 0;
    RadioSetupReply last{RadioSetupReply::Kind::Refused};
    const RadioSetupCompletion completed = [&](const RadioSetupReply& reply) { ++completions; last = reply; };
    check(sink("test", completed), "production sink reaches its injected lower writer");
    responses.back()(0, "accepted");
    check(completions == 1 && last.kind == RadioSetupReply::Kind::Accepted && last.message == "accepted",
          "production sink maps a zero radio code to acceptance");
    responses.back()(17, "failed");
    check(completions == 2 && last.kind == RadioSetupReply::Kind::Failed && last.code == 17 && last.message == "failed",
          "production sink retains negative response codes and bodies");
    RadioSetupControlTestAccess::closing(radio, true);
    check(!sink("closing", completed), "production sink refuses closing sessions");
    RadioSetupControlTestAccess::closing(radio, false);
    RadioSetupControlTestAccess::expiring(radio, true);
    check(!sink("expiring", completed), "production sink refuses callback-drain writes");
    RadioSetupControlTestAccess::expiring(radio, false);
    backend->live = false;
    check(!sink("disconnected", completed), "production sink refuses disconnected writes");
    backend->live = true;
    backend = installRadioSetupBackend(radio);
    check(!sink("replaced", completed) && writes == QStringList{"test"}, "production sink refuses retired backend generations before reaching the writer");

    backend->accept = false;
    QSignalSpy dropped(&radio, &RadioModel::commandDropped);
    for (const RadioSetupRequest& request : {RadioSetupRequest{RadioSetupRequest::Field::Callsign, QStringLiteral("N0CALL")},
             RadioSetupRequest{RadioSetupRequest::Field::ClientStation, QStringLiteral("Test Station")}}) {
        const ReceiveDispatch result = radio.requestRadioSetup(request, &radio, completed);
        check(result == ReceiveDispatch::LocalOnly && last.kind == RadioSetupReply::Kind::Accepted,
              "a refused radio copy retains an honest saved-locally identity receipt");
    }
    check(dropped.isEmpty() && radio.callsign() == "N0CALL"
        && AppSettings::instance().value("StationName").toString() == "Test Station",
          "radio-copy failure neither loses client identity nor spends the unsupported notice");

    const RadioControlSession old = radio.controlSession();
    RadioSetupControlTestAccess::replaceConnectionGeneration(radio);
    radio.requestTxBand({9, TxBandRequest::Field::RfPowerPercent, 37}, old);
    radio.requestTransverter({2, TransverterRequest::Field::RxOnly, true}, &radio, completed, old);
    check(dropped.isEmpty() && last.kind == RadioSetupReply::Kind::Cancelled, "stale editors cancel without blaming radio capability");
    RadioSetupControlTestAccess::retireBackend(radio);
    const ReceiveDispatch offline = radio.requestRadioSetup(
        {RadioSetupRequest::Field::ClientStation, QStringLiteral("Offline Station")}, &radio, completed);
    check(offline == ReceiveDispatch::LocalOnly && last.kind == RadioSetupReply::Kind::Accepted
        && dropped.isEmpty() && AppSettings::instance().value("StationName").toString() == "Offline Station",
        "client identity remains editable with no backend installed");
}

void usbRetirementCompletesBeforePublication()
{
    RadioModel radio;
    RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
    const auto retired = std::make_shared<bool>(false);
    backend->retired = retired;
    radio.usbCableModel().applyStatus("FTDI01", {{"type", "bit"}});
    const QMetaObject::Connection teardownObserver = QObject::connect(&radio.usbCableModel(), &UsbCableModel::cableRemoved, &radio, [&](const QString&) {
        check(*retired && !radio.backend(), "USB retirement observers see an already-retired backend");
        RadioSetupControlTestAccess::replaceConnectionGeneration(radio);
    });
    RadioSetupControlTestAccess::retireBackend(radio);
    check(*retired && !radio.backend(), "reentrant USB listeners cannot skip backend teardown");
    installRadioSetupBackend(radio);
    radio.usbCableModel().applyStatus("FTDI02", {{"type", "bit"}});
    RadioSetupControlTestAccess::pendingDisconnectState(radio);
    QObject::disconnect(teardownObserver);
    QObject::connect(&radio.usbCableModel(), &UsbCableModel::cableRemoved, &radio, [&](const QString&) {
        check(RadioSetupControlTestAccess::disconnectStateRetired(radio), "USB disconnect observers see cleared continuations and deferred pan writes");
        RadioSetupControlTestAccess::replaceConnectionGeneration(radio);
    });
    RadioSetupControlTestAccess::disconnected(radio);
    check(RadioSetupControlTestAccess::disconnectStateRetired(radio), "reentrant USB listeners cannot skip disconnect cleanup");
}

void invalidCallsignPreservesIdentity()
{
    using Field = RadioSetupRequest::Field;
    RadioModel radio;
    RadioSetupTestBackend* backend = installRadioSetupBackend(radio);
    radio.setStationCallsign(QStringLiteral("N0CALL"));
    const QStringList invalid{QString(), QStringLiteral(" "), QStringLiteral("#"),
        QStringLiteral("@_!"), QStringLiteral(".,/-"), QString::fromUtf8("雪"),
        QStringLiteral("N0CALL\ntransmit mox 1")};
    for (const bool connected : {true, false}) {
        backend->live = connected;
        for (const QString& input : invalid) {
            const RadioSetupRequest request{Field::Callsign, input};
            backend->commands.clear();
            check(!request.valid()
                && backend->requestRadioSetup(request) == ReceiveDispatch::Unsupported,
                "empty or symbol-only identity cannot reach the encoder");
            int replies = 0;
            RadioSetupReply last{RadioSetupReply::Kind::Accepted};
            const ReceiveDispatch result = radio.requestRadioSetup(request, &radio,
                [&](const RadioSetupReply& reply) { ++replies; last = reply; });
            QString persisted;
            check(result == ReceiveDispatch::Unsupported && replies == 1
                && last.kind == RadioSetupReply::Kind::Refused && backend->commands.isEmpty()
                && AppSettings::instance().readAppRowFromDisk(QStringLiteral("StationCallsign"), persisted)
                && persisted == QLatin1String("N0CALL")
                && radio.callsign() == QLatin1String("N0CALL"),
                "invalid connected or disconnected callsign preserves the database and emits no wire write");
        }
    }
    backend->live = true;
    backend->commands.clear();
    check(radio.requestRadioSetup({Field::Callsign, QStringLiteral("ki4ttz/p")})
        == ReceiveDispatch::Dispatched
        && backend->commands == QStringList{QStringLiteral("radio callsign KI4TTZ/P")},
        "a valid portable callsign still has its normal wire route");
    QString persisted;
    check(AppSettings::instance().readAppRowFromDisk(QStringLiteral("StationCallsign"), persisted)
        && persisted == QLatin1String("KI4TTZ/P"), "valid identity changes still persist");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-control-routing"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    radioEncoding();
    bandAndTransverterEncoding();
    usbEncoding();
    routingAndReplies();
    backendRefusals();
    reentrantCalibrationAndLocalSettings();
    invalidCallsignPreservesIdentity();
    calibrationAcknowledgements();
    productionSinkAndLocalReceipts();
    usbRetirementCompletesBeforePublication();
    return failures ? 1 : 0;
}
