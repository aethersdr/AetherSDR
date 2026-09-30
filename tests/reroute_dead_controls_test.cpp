// Controls that were silently dead on a radio with no Flex command plane
// (HL2, ANAN, Icom, RTL), although the radio could do what they ask through a
// path another control already uses. Each case below pins one reroute or one
// refusal at the model/seam level, against a backend with NO command plane and
// a call log in place of hardware.
//
// The Flex half of every change is pinned too, as wire text: where a caller
// used to write `slice set ...` by hand and now calls a SliceModel setter, the
// setter's commandReady output is compared with the exact strings the hand
// written version produced. That is the claim "a Flex sees the same commands",
// checked rather than asserted -- with one deliberate exception, pinned as
// such: a net filter stored with positive lower-sideband edges is now mirrored
// by setFilterWidth() before it is sent.
//
// Socket-free: an injected stub backend, an unopened RadioConnection where a
// command plane has to exist, and no transport, DSP or radio of any kind.
// Nothing is keyed.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/AutomationServer.h"
#include "core/BandStackSettings.h"
#include "core/RigctlProtocol.h"
#include "core/SmartCatProtocol.h"
#include "core/TciProtocol.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/MeterDef.h"
#include "core/backends/flex/RadioConnection.h"
#include "models/EqualizerModel.h"
#include "models/MeterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStringList>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace AetherSDR {
class RerouteDeadControlsTestAccess
{
public:
    // An unopened RadioConnection: no socket, no thread, no peer. Its presence
    // is exactly what hasCommandPlane() asks about.
    static void useCommandPlane(RadioModel& radio, RadioConnection* connection)
    {
        radio.m_connection = connection;
    }
};

// handleLine() is private; AutomationServer befriends this name for its tests.
class AutomationServerTestAccess
{
public:
    static QJsonObject handleLine(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int g_failed = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failed;
    }
}

// The HL2's shape where it matters: no command plane, host noise blanker, no
// radio-side DSP, no AM carrier control. Individual cases flip one field.
class LoggingBackend final : public IRadioBackend
{
public:
    RadioCapabilities caps;
    bool connected{true};

    struct Agc { int slice; QString mode; int threshold; };
    struct Toggle { int slice; bool on; int level; };
    std::vector<Agc> agc;
    std::vector<Toggle> nb;
    std::vector<Toggle> nr;
    std::vector<Toggle> anf;
    std::vector<Toggle> filter;   // on unused; level = low, stored twice below
    std::vector<int> filterHigh;

    LoggingBackend()
    {
        caps.hasRadioSideDsp = false;
        caps.hasHostNoiseBlanker = true;
        caps.hasAmCarrierLevel = false;
    }

    RadioCapabilities capabilities() const override { return caps; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; }
    bool isConnected() const override { return connected; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int s, int lo, int hi) override
    {
        filter.push_back({s, true, lo});
        filterHigh.push_back(hi);
    }
    void setSliceAgc(int s, const QString& mode, int threshold) override
    {
        agc.push_back({s, mode, threshold});
    }
    void setSliceNoiseBlanker(int s, bool on, int level) override { nb.push_back({s, on, level}); }
    void setSliceNoiseReduction(int s, bool on, int level) override { nr.push_back({s, on, level}); }
    void setSliceAutoNotch(int s, bool on) override { anf.push_back({s, on, 0}); }
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

struct Fixture {
    RadioModel radio;
    LoggingBackend* backend{nullptr};
    SliceModel* slice{nullptr};

    explicit Fixture(void (*configure)(RadioCapabilities&) = nullptr)
    {
        auto owned = std::make_unique<LoggingBackend>();
        backend = owned.get();
        if (configure) {
            configure(backend->caps);
        }
        radio.setBackendForTest(std::move(owned), QStringLiteral("reroute-test"));
        // A pan first, so the slice's first delta lands on a pane that exists;
        // the first delta then materialises the slice through the production
        // non-Flex path, with every seam intent it wires.
        const QString opaquePan = QStringLiteral("reroute/pan0");
        emit backend->panCenterBandwidthChanged(opaquePan, 14.2, 0.1);
        SliceDelta delta;
        delta.panId = opaquePan;
        delta.frequency = 14.2;
        delta.mode = QStringLiteral("USB");
        delta.filterLow = 100;
        delta.filterHigh = 2900;
        delta.inUse = true;
        emit backend->sliceChanged(0, delta);
        slice = radio.slice(0);
        if (!slice) {
            std::fprintf(stderr, "FATAL: the non-Flex slice was not materialised\n");
            std::exit(2);
        }
    }
};

QStringList wireOf(const QSignalSpy& spy)
{
    QStringList out;
    for (const QList<QVariant>& args : spy) {
        out << args.at(0).toString();
    }
    return out;
}

// ── Row 2, 14 (step half) and the RX applet STEP: a client-owned step ──────

void testStepIsClientOwnedWithoutCommandPlane()
{
    Fixture f;
    QSignalSpy dropped(&f.radio, &RadioModel::commandDropped);
    QSignalSpy stepChanged(f.slice, &SliceModel::stepChanged);
    check(!f.radio.hasCommandPlane(), "step: the fixture has no command plane");
    check(f.radio.applyClientOwnedSliceStep(0, 500),
          "step: handled on the client when there is no command plane");
    check(f.slice->stepHz() == 500, "step: the slice's step is the one asked for");
    check(stepChanged.size() == 1,
          "step: stepChanged fires, which the RX applet and tuning wheel follow");
    check(dropped.isEmpty(), "step: nothing is dropped, so no false 'unsupported' notice");
}

void testStepStaysRadioOwnedWithCommandPlane()
{
    Fixture f;
    RadioConnection unopened;
    RerouteDeadControlsTestAccess::useCommandPlane(f.radio, &unopened);
    const int before = f.slice->stepHz();
    check(f.radio.hasCommandPlane(), "step/flex: the fixture has a command plane");
    check(!f.radio.applyClientOwnedSliceStep(0, 500),
          "step/flex: declined, so the caller sends its wire text as before");
    check(f.slice->stepHz() == before,
          "step/flex: the client does not assert a radio-owned step (Principle II)");
    RerouteDeadControlsTestAccess::useCommandPlane(f.radio, nullptr);
}

void testRigctlSetTsReachesTheSlice()
{
    Fixture f;
    QSignalSpy dropped(&f.radio, &RadioModel::commandDropped);
    RigctlProtocol port(&f.radio);
    port.setSliceIndex(0);
    const QString reply = port.handleLine(QStringLiteral("\\set_ts 1000")).trimmed();
    QCoreApplication::processEvents();   // set_ts applies on a queued hop
    check(reply == QLatin1String("RPRT 0"), "CAT set_ts: answered RPRT 0");
    check(f.slice->stepHz() == 1000,
          "CAT set_ts: the slice step is what the client asked for (was: dropped)");
    check(dropped.isEmpty(), "CAT set_ts: no command was dropped");
    const QString readBack = port.handleLine(QStringLiteral("\\get_ts")).trimmed();
    check(readBack == QLatin1String("1000"), "CAT get_ts reads back the step set_ts set");
}

// ── Rows 3, 4, 8: the radio's own NR / ANF ─────────────────────────────────

void testRadioNrAndAnfRefuseWithoutRadioDsp()
{
    Fixture f;
    check(!f.radio.radioSideNoiseReductionAvailable(),
          "NR: a radio with no radio-side DSP reports none (nr_cycle skips its NR step)");
    check(!f.radio.requestRadioNoiseReduction(f.slice, true),
          "NR: a MIDI/controller request is refused, for the caller to announce");
    check(!f.slice->nrOn(), "NR: the model is not marked on (no phantom 'NR on')");
    check(f.backend->nr.empty(), "NR: nothing reached the seam");
    check(!f.radio.requestRadioAutoNotch(f.slice, true), "ANF: refused the same way");
    check(!f.slice->anfOn() && f.backend->anf.empty(), "ANF: no phantom, nothing sent");
}

void testRadioNrAndAnfRouteWhereTheRadioHasThem()
{
    // Icom's shape: radio-side NR and ANF behind seam verbs.
    Fixture f([](RadioCapabilities& c) { c.hasRadioSideDsp = true; });
    check(f.radio.radioSideNoiseReductionAvailable(), "NR/icom: available");
    check(f.radio.requestRadioNoiseReduction(f.slice, true), "NR/icom: accepted");
    check(f.slice->nrOn() && f.backend->nr.size() == 1 && f.backend->nr.back().on,
          "NR/icom: reaches setSliceNoiseReduction");
    check(f.radio.requestRadioAutoNotch(f.slice, true), "ANF/icom: accepted");
    check(f.backend->anf.size() == 1 && f.backend->anf.back().on,
          "ANF/icom: reaches setSliceAutoNotch");
}

void testAnfStillReachesTheDemoCommandPlane()
{
    // The Demo radio's shape: a (synthetic) command plane and no radio-side
    // DSP. Its connection answers `anf=` with the generator's audible notch,
    // so ANF must not be refused there; it has no NR, which still is.
    Fixture f;
    RadioConnection unopened;
    RerouteDeadControlsTestAccess::useCommandPlane(f.radio, &unopened);
    QSignalSpy wire(f.slice, &SliceModel::commandReady);
    check(f.radio.requestRadioAutoNotch(f.slice, true),
          "ANF/demo: accepted where a command plane carries `anf=`");
    check(wireOf(wire) == QStringList{QStringLiteral("slice set 0 anf=1")},
          "ANF/demo: the same wire text as before");
    check(!f.radio.requestRadioNoiseReduction(f.slice, true) && !f.slice->nrOn(),
          "NR/demo: still refused, no phantom");
    RerouteDeadControlsTestAccess::useCommandPlane(f.radio, nullptr);
}

// The remote-control surfaces reach SliceModel::setNr/setAnf too: CAT
// (rigctld set_func, SmartSDR-CAT ZZNR/NR/NT), TCI and the automation bridge.
// Each refuses ON in its own protocol's terms where the radio has no
// radio-side NR/ANF, leaving no phantom "on"; OFF, already the truth there, is
// answered as before; and a radio that has them is unchanged.
void testRemoteSurfacesRefuseRadioNrAndAnfWithoutRadioDsp()
{
    Fixture f;
    auto noPhantom = [&f]() {
        QCoreApplication::processEvents();   // rigctl/TCI apply on a queued hop
        auto anyOn = [](const std::vector<LoggingBackend::Toggle>& log) {
            return std::any_of(log.begin(), log.end(),
                               [](const LoggingBackend::Toggle& t) { return t.on; });
        };
        return !f.slice->nrOn() && !f.slice->anfOn()
            && !anyOn(f.backend->nr) && !anyOn(f.backend->anf);
    };

    RigctlProtocol rig(&f.radio);
    rig.setSliceIndex(0);
    check(rig.handleLine(QStringLiteral("\\set_func NR 1")).trimmed() == QLatin1String("RPRT -11"),
          "remote/rigctl: set_func NR 1 answers RIG_ENAVAIL (was: RPRT 0)");
    check(rig.handleLine(QStringLiteral("\\set_func ANF 1")).trimmed() == QLatin1String("RPRT -11"),
          "remote/rigctl: set_func ANF 1 answers RIG_ENAVAIL");
    check(noPhantom(), "remote/rigctl: no phantom NR/ANF, no ON reached the seam");
    check(rig.handleLine(QStringLiteral("\\set_func NR 0")).trimmed() == QLatin1String("RPRT 0"),
          "remote/rigctl: set_func NR 0 is still answered RPRT 0");

    TciProtocol tci(&f.radio);
    (void)tci.handleCommand(QStringLiteral("rx_nr_enable:0,true"));
    check(tci.pendingNotification() == QLatin1String("rx_nr_enable:0,false;"),
          "remote/TCI: rx_nr_enable true is answered with the truth, false");
    (void)tci.handleCommand(QStringLiteral("rx_anf_enable:0,true"));
    check(tci.pendingNotification() == QLatin1String("rx_anf_enable:0,false;"),
          "remote/TCI: rx_anf_enable true is answered with the truth, false");
    check(noPhantom(), "remote/TCI: no phantom NR/ANF, no ON reached the seam");

    SmartCatProtocol cat(&f.radio, 0);
    check(cat.processCommand(QStringLiteral("ZZNR1")) == QLatin1String("?;"),
          "remote/SmartCAT: ZZNR1 refused with ?;");
    check(cat.processCommand(QStringLiteral("NR1")) == QLatin1String("?;"),
          "remote/SmartCAT: NR1 refused with ?;");
    check(cat.processCommand(QStringLiteral("NT1")) == QLatin1String("?;"),
          "remote/SmartCAT: NT1 refused with ?;");
    check(noPhantom(), "remote/SmartCAT: no phantom NR/ANF, no ON reached the seam");
    check(cat.processCommand(QStringLiteral("NR0")).isEmpty(),
          "remote/SmartCAT: NR0 is accepted as before");

    AutomationServer bridge;
    bridge.setRadioModel(&f.radio);
    const QJsonObject nr =
        AutomationServerTestAccess::handleLine(bridge, QByteArrayLiteral("slice dsp nr on"));
    const QJsonObject anf =
        AutomationServerTestAccess::handleLine(bridge, QByteArrayLiteral("slice dsp anf on"));
    check(!nr.value(QStringLiteral("ok")).toBool() && !anf.value(QStringLiteral("ok")).toBool(),
          "remote/bridge: slice dsp nr|anf on is refused");
    check(noPhantom(), "remote/bridge: no phantom NR/ANF, no ON reached the seam");
}

void testRemoteSurfacesStillReachRadioNrWhereItExists()
{
    Fixture f([](RadioCapabilities& c) { c.hasRadioSideDsp = true; });
    RigctlProtocol rig(&f.radio);
    rig.setSliceIndex(0);
    check(rig.handleLine(QStringLiteral("\\set_func NR 1")).trimmed() == QLatin1String("RPRT 0"),
          "remote/icom: set_func NR 1 accepted");
    TciProtocol tci(&f.radio);
    (void)tci.handleCommand(QStringLiteral("rx_anf_enable:0,true"));
    check(tci.pendingNotification() == QLatin1String("rx_anf_enable:0,true;"),
          "remote/icom: TCI rx_anf_enable accepted as before");
    QCoreApplication::processEvents();
    check(f.slice->nrOn() && !f.backend->nr.empty() && f.backend->nr.back().on,
          "remote/icom: NR reaches setSliceNoiseReduction");
    check(f.slice->anfOn() && !f.backend->anf.empty() && f.backend->anf.back().on,
          "remote/icom: ANF reaches setSliceAutoNotch");
}

// ── Row 5: AM carrier ───────────────────────────────────────────────────────

void testAmCarrierFollowsTheCapability()
{
    {
        Fixture f;
        QSignalSpy wire(&f.radio.transmitModel(), &TransmitModel::commandReady);
        const int before = f.radio.transmitModel().amCarrierLevel();
        check(!f.radio.requestAmCarrierLevel(before == 30 ? 31 : 30),
              "AM carrier: refused on a radio that declares no AM carrier control");
        check(f.radio.transmitModel().amCarrierLevel() == before && wire.isEmpty(),
              "AM carrier: no optimistic value, no wire text");
    }
    {
        Fixture f([](RadioCapabilities& c) { c.hasAmCarrierLevel = true; });
        QSignalSpy wire(&f.radio.transmitModel(), &TransmitModel::commandReady);
        check(f.radio.requestAmCarrierLevel(30), "AM carrier/flex: accepted where declared");
        check(wireOf(wire) == QStringList{QStringLiteral("transmit set am_carrier=30")},
              "AM carrier/flex: the same wire text as before");
    }
}

// ── Row 6: graphic EQ ───────────────────────────────────────────────────────

void testGraphicEqIsNotReportedAsUnsupported()
{
    Fixture f;
    QSignalSpy dropped(&f.radio, &RadioModel::commandDropped);
    QSignalSpy txState(&f.radio.equalizerModel(), &EqualizerModel::txStateChanged);
    f.radio.equalizerModel().setTxEnabled(true);
    f.radio.equalizerModel().setTxBand(EqualizerModel::B1k, 5);
    check(f.radio.equalizerModel().txBand(EqualizerModel::B1k) == 5,
          "EQ: the model holds the band a MIDI knob set");
    check(txState.size() == 2,
          "EQ: txStateChanged fires, which drives the ClientEq mapping");
    check(dropped.isEmpty(),
          "EQ: the Flex `eq` text is not sent into a drop that says 'unsupported'");
}

// ── Row 11: the S-meter's TX Level face ─────────────────────────────────────

MeterDef txMeter(int index, const char* name)
{
    MeterDef d;
    d.index = index;
    d.source = QStringLiteral("TX");
    d.name = QString::fromLatin1(name);
    d.unit = QStringLiteral("dBFS");
    d.low = -100.0;
    d.high = 0.0;
    return d;
}

void testLevelFaceReadsMicPeakWhereThereIsNoMicMeter()
{
    MeterModel hl2;
    hl2.defineMeter(txMeter(6, "MICPEAK"));   // Hl2Backend's meter 6, and no MIC
    check(!hl2.hasMicLevelMeter() && hl2.hasMicPeakMeter(),
          "S-meter: the HL2 shape defines MICPEAK only");
    check(hl2.transmitLevelFaceValue(-50.0f, -12.5f) == -12.5f,
          "S-meter: the Level face reads MICPEAK there (was: pinned at -50)");

    MeterModel flex;
    flex.defineMeter(txMeter(20, "MICPEAK"));
    flex.defineMeter(txMeter(21, "MIC"));
    check(flex.transmitLevelFaceValue(-31.0f, -12.5f) == -31.0f,
          "S-meter/flex: a radio defining MIC still shows MIC");

    MeterModel none;
    check(none.transmitLevelFaceValue(-50.0f, -12.5f) == -50.0f,
          "S-meter: a radio with neither meter is unchanged");
}

// Every consumer of micMetersChanged shows a "transmit level": the S-meter's
// Level face, the VFO SmartMTR, the Phone/CW Level gauge and TCI tx_sensors.
// They must agree, so each has to pass the level through the one helper. Read
// as source text because the three GUI consumers are lambdas in MainWindow,
// which this test cannot construct; a new consumer that forgets the helper
// fails here too.
QString readSource(const QString& relative)
{
    QFile file(QStringLiteral(AETHER_SOURCE_DIR "/") + relative);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

void testEveryTransmitLevelConsumerUsesTheHelper()
{
    const QString marker = QStringLiteral("&MeterModel::micMetersChanged");
    int consumers = 0;
    int usingHelper = 0;
    QDirIterator it(QStringLiteral(AETHER_SOURCE_DIR "/src"), {QStringLiteral("*.cpp")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        const QString text = QString::fromUtf8(file.readAll());
        for (qsizetype at = text.indexOf(marker); at >= 0;
             at = text.indexOf(marker, at + marker.size())) {
            const qsizetype end = text.indexOf(QStringLiteral("});"), at);
            const QString body = text.mid(at, end < 0 ? -1 : end - at);
            ++consumers;
            if (body.contains(QStringLiteral("transmitLevelFaceValue("))) {
                ++usingHelper;
            } else {
                std::printf("       no transmitLevelFaceValue in the consumer at %s\n",
                            qPrintable(path));
            }
        }
    }
    check(consumers >= 4, "level face: the four micMetersChanged consumers are found");
    check(consumers == usingHelper,
          "level face: every consumer shows MICPEAK where there is no MIC (VFO, Phone/CW, TCI too)");
}

// ── Rows 12/13: band-stack bookmark recall ──────────────────────────────────

BandStackEntry bookmark()
{
    BandStackEntry e;
    e.agcMode = QStringLiteral("fast");
    e.agcThreshold = 70;
    e.nbOn = true;
    e.nbLevel = 30;
    e.nrOn = true;
    e.nrLevel = 40;
    return e;
}

void testBandStackRecallReachesTheSeam()
{
    Fixture f;
    QSignalSpy dropped(&f.radio, &RadioModel::commandDropped);
    f.radio.recallBandStackReceiveDsp(f.slice, bookmark());
    check(!f.backend->agc.empty() && f.backend->agc.back().mode == QLatin1String("fast")
              && f.backend->agc.back().threshold == 70,
          "band stack: AGC mode and threshold reach setSliceAgc (was: dropped)");
    check(!f.backend->nb.empty() && f.backend->nb.back().on && f.backend->nb.back().level == 30,
          "band stack: NB on and level reach setSliceNoiseBlanker (was: dropped)");
    check(f.backend->nr.empty() && !f.slice->nrOn(),
          "band stack: NR is not planted on a radio with no radio-side NR");
    check(dropped.isEmpty(), "band stack: nothing dropped, no false notice");
}

void testBandStackRecallWritesTheSameFlexWireText()
{
    // Radio-side DSP declared, as a Flex does. The slice's commandReady is
    // what the Flex slice sink forwards; compare it with the text the recall
    // used to write by hand, in the order it wrote it.
    Fixture f([](RadioCapabilities& c) { c.hasRadioSideDsp = true; });
    QSignalSpy wire(f.slice, &SliceModel::commandReady);
    f.radio.recallBandStackReceiveDsp(f.slice, bookmark());
    const QStringList expected{
        QStringLiteral("slice set 0 agc_mode=fast"),
        QStringLiteral("slice set 0 agc_threshold=70"),
        QStringLiteral("slice set 0 nb=1"),
        QStringLiteral("slice set 0 nb_level=30"),
        QStringLiteral("slice set 0 nr=1"),
        QStringLiteral("slice set 0 nr_level=40"),
    };
    check(wireOf(wire) == expected,
          "band stack/flex: byte-for-byte the wire text the hand-written recall sent");

    // And a recall that matches the slice sends nothing, as before.
    wire.clear();
    f.radio.recallBandStackReceiveDsp(f.slice, bookmark());
    check(wire.isEmpty(), "band stack/flex: an already-matching recall sends nothing");
}

void testBandStackRecallLeavesTheKiwiAgcAlone()
{
    // While KiwiSDR external receive audio replaces a slice, SliceModel's AGC
    // setters write the KiwiSDR AGC. The bookmark holds the RADIO's AGC, so the
    // recall must not route it there; MainWindow sends it as the hand-written
    // `slice set` text it always sent.
    Fixture f([](RadioCapabilities& c) { c.hasRadioSideDsp = true; });
    f.slice->setExternalReceiveAudioReplacementMute(true);
    const QString kiwiMode = f.slice->receiveAgcMode();
    const int kiwiThreshold = f.slice->receiveAgcThreshold();
    f.backend->agc.clear();
    QSignalSpy wire(f.slice, &SliceModel::commandReady);
    f.radio.recallBandStackReceiveDsp(f.slice, bookmark());
    check(f.slice->receiveAgcMode() == kiwiMode
              && f.slice->receiveAgcThreshold() == kiwiThreshold,
          "band stack/kiwi: the KiwiSDR AGC is not overwritten with the radio's");
    check(f.backend->agc.empty(), "band stack/kiwi: no AGC intent from the recall");
    const QStringList expected{
        QStringLiteral("slice set 0 nb=1"),
        QStringLiteral("slice set 0 nb_level=30"),
        QStringLiteral("slice set 0 nr=1"),
        QStringLiteral("slice set 0 nr_level=40"),
    };
    check(wireOf(wire) == expected, "band stack/kiwi: NB and NR text as before, no AGC text");

    // ...and the caller still writes the radio's AGC as it did before.
    const QString mw = readSource(QStringLiteral("src/gui/MainWindow.cpp"));
    const qsizetype recall = mw.indexOf(QStringLiteral("recallBandStackReceiveDsp(slice, e)"));
    const qsizetype start = mw.lastIndexOf(QStringLiteral("BandStackPanel::recallRequested"), recall);
    const QString lambda = (recall > 0 && start > 0) ? mw.mid(start, recall - start) : QString();
    check(lambda.contains(QStringLiteral("if (slice->externalReceiveReplacementActive())"))
              && lambda.contains(QStringLiteral(
                  "QString(\"slice set %1 agc_mode=%2\").arg(id).arg(e.agcMode)"))
              && lambda.contains(QStringLiteral(
                  "QString(\"slice set %1 agc_threshold=%2\").arg(id).arg(e.agcThreshold)")),
          "band stack/kiwi: MainWindow sends the radio AGC text the recall always sent");
}

// ── Row 14 (filter half): a net's filter on Tune Now ────────────────────────

void testNetFilterReachesTheSeam()
{
    // tuneToNet now calls SliceModel::setFilterWidth instead of writing `filt`.
    Fixture f;
    QSignalSpy wire(f.slice, &SliceModel::commandReady);
    f.slice->setFilterWidth(200, 2600);
    check(!f.backend->filter.empty() && f.backend->filter.back().level == 200
              && f.backend->filterHigh.back() == 2600,
          "net Tune Now: the filter reaches setSliceFilter (was: `filt` dropped)");
    check(wireOf(wire) == QStringList{QStringLiteral("filt 0 200 2600")},
          "net Tune Now/flex: the same `filt` text a Flex received before");
}

void testNetFilterIsNormalisedForLowerSideband()
{
    // The one place the Flex wire text of this branch DIFFERS from before, on
    // purpose. A net preset can carry lower-sideband edges stored positive
    // (the pre-mirror convention, #3434). The hand-written `filt` sent them
    // as stored; setFilterWidth() mirrors them across the carrier first, the
    // same normalisation every other filter caller already gets. Canonical
    // (negative) LSB edges pass through unchanged.
    Fixture f;
    f.slice->setMode(QStringLiteral("LSB"));
    QSignalSpy wire(f.slice, &SliceModel::commandReady);
    f.slice->setFilterWidth(100, 2900);
    check(wireOf(wire) == QStringList{QStringLiteral("filt 0 -2900 -100")},
          "net Tune Now/LSB: positive stored edges are mirrored (raw text sent `filt 0 100 2900`)");
    check(f.slice->filterLow() == -2900 && f.slice->filterHigh() == -100,
          "net Tune Now/LSB: the model holds the canonical edges");
    check(!f.backend->filter.empty() && f.backend->filter.back().level == -2900
              && f.backend->filterHigh.back() == -100,
          "net Tune Now/LSB: the seam receives the canonical edges");

    wire.clear();
    f.slice->setFilterWidth(-2700, -300);
    check(wireOf(wire) == QStringList{QStringLiteral("filt 0 -2700 -300")},
          "net Tune Now/LSB: canonical edges are sent exactly as stored");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-reroute-dead-controls-test"));
    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "isolated settings profile is available");
    AppSettings::instance().load();

    testStepIsClientOwnedWithoutCommandPlane();
    testStepStaysRadioOwnedWithCommandPlane();
    testRigctlSetTsReachesTheSlice();
    testRadioNrAndAnfRefuseWithoutRadioDsp();
    testRadioNrAndAnfRouteWhereTheRadioHasThem();
    testAnfStillReachesTheDemoCommandPlane();
    testRemoteSurfacesRefuseRadioNrAndAnfWithoutRadioDsp();
    testRemoteSurfacesStillReachRadioNrWhereItExists();
    testAmCarrierFollowsTheCapability();
    testGraphicEqIsNotReportedAsUnsupported();
    testLevelFaceReadsMicPeakWhereThereIsNoMicMeter();
    testEveryTransmitLevelConsumerUsesTheHelper();
    testBandStackRecallReachesTheSeam();
    testBandStackRecallWritesTheSameFlexWireText();
    testBandStackRecallLeavesTheKiwiAgcAlone();
    testNetFilterReachesTheSeam();
    testNetFilterIsNormalisedForLowerSideband();

    if (g_failed == 0) {
        std::printf("reroute_dead_controls_test: all checks passed\n");
        return 0;
    }
    std::printf("reroute_dead_controls_test: %d failure(s)\n", g_failed);
    return 1;
}
