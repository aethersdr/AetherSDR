// SmartSDR 4.3 FreeDV mode tokens over CAT (#6653). FlexLib v4.3.16 replaced
// the bare FDV token with FDVU/FDVL and treats FDVL as lower sideband
// (Slice.cs:799,825). SmartCAT and rigctld must report both, accept them back
// without leaving FreeDV, and place an FDVL passband on the lower side; legacy
// FDV keeps its old answers.
//
// Socket-free: an injected stub backend that records slice intents, and the
// protocol objects driven by command text. Nothing is keyed.

#include "TestSettingsProfile.h"
#include "core/RigctlProtocol.h"
#include "core/SmartCatProtocol.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>

#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

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

class RecordingBackend final : public IRadioBackend
{
public:
    struct Filter { int lo; int hi; };
    QStringList modes;
    std::vector<Filter> filters;

    RadioCapabilities capabilities() const override { return {}; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    bool isConnected() const override { return true; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString& mode) override { modes.append(mode); }
    void setSliceFilter(int, int lo, int hi) override { filters.push_back({lo, hi}); }
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

struct Fixture {
    RadioModel radio;
    RecordingBackend* backend{nullptr};
    SliceModel* slice{nullptr};

    Fixture(const QString& mode, const QStringList& modeList)
    {
        auto owned = std::make_unique<RecordingBackend>();
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("cat-freedv-test"));
        const QString pan = QStringLiteral("cat-freedv/pan0");
        emit backend->panCenterBandwidthChanged(pan, 14.236, 0.1);
        SliceDelta delta;
        delta.panId = pan;
        delta.frequency = 14.236;
        delta.mode = mode;
        delta.modeList = modeList;
        delta.filterLow = 100;
        delta.filterHigh = 2900;
        delta.inUse = true;
        emit backend->sliceChanged(0, delta);
        slice = radio.slice(0);
        if (!slice) {
            std::fprintf(stderr, "FATAL: the slice was not materialised\n");
            std::exit(2);
        }
        backend->modes.clear();
        backend->filters.clear();
    }
};

const QStringList kModes43{"LSB", "USB", "CW", "AM", "DIGU", "DIGL", "FDVU", "FDVL"};
const QStringList kModesLegacy{"LSB", "USB", "CW", "AM", "DIGU", "DIGL", "FDV"};

void testSmartCatReportsModernTokens()
{
    check(SmartCatProtocol::modeToKenwood("FDVU") == "9", "SmartCAT MD: FDVU reports 9 (DIGU)");
    check(SmartCatProtocol::modeToKenwood("FDVL") == "6", "SmartCAT MD: FDVL reports 6 (DIGL)");
    check(SmartCatProtocol::modeToKenwood("FDV") == "9", "SmartCAT MD: legacy FDV still reports 9");
    check(SmartCatProtocol::modeToZZ("FDVU") == "20", "SmartCAT ZZMD: FDVU reports 20");
    check(SmartCatProtocol::modeToZZ("FDVL") == "20", "SmartCAT ZZMD: FDVL reports 20");
    check(SmartCatProtocol::modeToZZ("FDV") == "20", "SmartCAT ZZMD: legacy FDV still reports 20");

    Fixture f(QStringLiteral("FDVL"), kModes43);
    SmartCatProtocol cat(&f.radio, 0, 1, true);
    check(cat.processCommand("ZZMD") == "ZZMD20;", "SmartCAT ZZMD; on an FDVL slice answers 20");
    check(cat.processCommand("MD") == "MD6;", "SmartCAT MD; on an FDVL slice answers 6");
}

void testSmartCatZz20PicksTheRadiosToken()
{
    {
        Fixture f(QStringLiteral("USB"), kModes43);
        SmartCatProtocol cat(&f.radio, 0, 1, true);
        cat.processCommand("ZZMD20");
        QCoreApplication::processEvents();
        check(f.backend->modes == QStringList{"FDVU"},
              "SmartCAT ZZMD20 on 4.3: sets FDVU, the token the mode_list offers");
    }
    {
        Fixture f(QStringLiteral("USB"), kModesLegacy);
        SmartCatProtocol cat(&f.radio, 0, 1, true);
        cat.processCommand("ZZMD20");
        QCoreApplication::processEvents();
        check(f.backend->modes == QStringList{"FDV"},
              "SmartCAT ZZMD20 on older firmware: still sets FDV");
    }
    {
        Fixture f(QStringLiteral("FDVL"), kModes43);
        SmartCatProtocol cat(&f.radio, 0, 1, true);
        cat.processCommand("ZZMD20");
        cat.processCommand("MD6");
        QCoreApplication::processEvents();
        check(f.backend->modes.isEmpty(),
              "SmartCAT ZZMD20 / MD6 echoed back on FDVL: the slice stays FDVL");
        check(f.slice->mode() == "FDVL", "SmartCAT echo: slice mode is still FDVL");
    }
    {
        Fixture f(QStringLiteral("FDVU"), kModes43);
        SmartCatProtocol cat(&f.radio, 0, 1, true);
        cat.processCommand("MD6");
        QCoreApplication::processEvents();
        check(f.backend->modes == QStringList{"DIGL"},
              "SmartCAT MD6 on FDVU: a different code still changes the mode");
    }
}

void testRigctlReportsAndKeepsFreeDv()
{
    Fixture f(QStringLiteral("FDVL"), kModes43);
    RigctlProtocol rig(&f.radio);
    rig.setSliceIndex(0);
    const QString reply = rig.handleLine(QStringLiteral("m"));
    check(reply.startsWith(QLatin1String("PKTLSB\n")), "rigctld m on FDVL: answers PKTLSB");

    rig.handleLine(QStringLiteral("M PKTLSB 2400"));
    QCoreApplication::processEvents();
    check(f.backend->modes.isEmpty(), "rigctld M PKTLSB on FDVL: the slice stays FDVL");
    check(!f.backend->filters.empty() && f.backend->filters.back().lo == -2400
              && f.backend->filters.back().hi == -95,
          "rigctld M PKTLSB 2400 on FDVL: passband on the lower side (-2400..-95)");

    Fixture u(QStringLiteral("FDVU"), kModes43);
    RigctlProtocol rigU(&u.radio);
    rigU.setSliceIndex(0);
    check(rigU.handleLine(QStringLiteral("m")).startsWith(QLatin1String("PKTUSB\n")),
          "rigctld m on FDVU: answers PKTUSB");
    rigU.handleLine(QStringLiteral("M PKTUSB 2400"));
    QCoreApplication::processEvents();
    check(u.backend->modes.isEmpty(), "rigctld M PKTUSB on FDVU: the slice stays FDVU");
    check(!u.backend->filters.empty() && u.backend->filters.back().lo == 95
              && u.backend->filters.back().hi == 2400,
          "rigctld M PKTUSB 2400 on FDVU: passband on the upper side (95..2400)");

    // The model also mirrors positive lower-sideband edges on the non-Flex
    // path, so pin the edges rigctld itself computes.
    using Edges = std::pair<int, int>;
    check(RigctlProtocol::passbandEdges("FDVL", 2400) == Edges(-2400, -95),
          "rigctld passband: FDVL edges are lower-sideband");
    check(RigctlProtocol::passbandEdges("FDVU", 2400) == Edges(95, 2400),
          "rigctld passband: FDVU edges are upper-sideband");
    check(RigctlProtocol::passbandEdges("DIGL", 2400) == Edges(-2400, -95),
          "rigctld passband: DIGL unchanged");

    Fixture d(QStringLiteral("DIGU"), kModes43);
    RigctlProtocol rigD(&d.radio);
    rigD.setSliceIndex(0);
    rigD.handleLine(QStringLiteral("M PKTLSB 0"));
    QCoreApplication::processEvents();
    check(d.backend->modes == QStringList{"DIGL"}, "rigctld M PKTLSB on DIGU: still sets DIGL");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("cat-freedv-mode-tokens-test"));
    QCoreApplication app(argc, argv);
    testSmartCatReportsModernTokens();
    testSmartCatZz20PicksTheRadiosToken();
    testRigctlReportsAndKeepsFreeDv();
    std::printf("%s\n", g_failed == 0 ? "ALL PASSED" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
