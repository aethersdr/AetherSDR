// A band-stack recall into DIGU/DIGL leaves the Hermes-Lite 2's data-mode AGC
// alone (#6142). Driven in MainWindow's recall order: SliceModel::setMode(),
// then RadioModel::recallBandStackReceiveDsp(), against a real Hl2Backend.
// Socket-free: the backend is never connected, so there is no Metis
// transport, no DSP chain and no radio. Nothing is keyed.

#include "TestSettingsProfile.h"
#include "core/BandStackSettings.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

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

// RadioModel dispatches receive intents only to a connected backend, and
// installs a slice fixture only on a disconnected one.
class LinkedHl2Backend final : public hl2::Hl2Backend
{
public:
    bool linked{false};
    bool isConnected() const override { return linked; }
};

BandStackEntry bookmark(const char* mode, const char* agcMode, int agcThreshold)
{
    BandStackEntry e;
    e.mode = QString::fromLatin1(mode);
    e.agcMode = QString::fromLatin1(agcMode);
    e.agcThreshold = agcThreshold;
    return e;
}

// MainWindow's BandStackPanel::recallRequested lambda, AGC-relevant steps in
// its order: the mode first, then the receive DSP.
void recall(RadioModel& radio, SliceModel* slice, const BandStackEntry& e)
{
    if (slice->mode() != e.mode) {
        slice->setMode(e.mode);
    }
    radio.recallBandStackReceiveDsp(slice, e);
}

struct Hl2Fixture {
    RadioModel radio;
    hl2::Hl2Backend* backend{nullptr};
    SliceModel* slice{nullptr};
    // What the receiver runs, as the backend publishes it.
    QString agc;
    int threshold{-1};

    Hl2Fixture()
    {
        auto owned = std::make_unique<LinkedHl2Backend>();
        LinkedHl2Backend* linked = owned.get();
        backend = linked;
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        if (!radio.automationApplySliceFixture(0, QStringLiteral("A")) || !radio.slice(0)) {
            std::fprintf(stderr, "FATAL: the slice fixture was not installed\n");
            std::exit(2);
        }
        slice = radio.slice(0);
        linked->linked = true;
        QObject::connect(backend, &IRadioBackend::sliceChanged, backend,
                         [this](int, const SliceDelta& d) {
            if (d.agcMode) {
                agc = *d.agcMode;
            }
            if (d.agcThreshold) {
                threshold = *d.agcThreshold;
            }
        });
    }

    // The operator's own state before a recall: a voice mode and its AGC.
    void operate(const char* mode, const char* agcMode)
    {
        slice->setMode(QString::fromLatin1(mode));
        slice->setAgcMode(QString::fromLatin1(agcMode));
    }

    bool runs(const char* agcMode) const
    {
        return agc == QLatin1String(agcMode) && slice->agcMode() == QLatin1String(agcMode);
    }
};

void testStaleDataModeBookmark()
{
    Hl2Fixture f;
    f.operate("LSB", "slow");
    check(f.runs("slow"), "fixture: the operator's AGC reaches the receiver in LSB");

    recall(f.radio, f.slice, bookmark("DIGU", "med", 65));
    check(f.slice->mode() == QLatin1String("DIGU"), "the recall selects DIGU");
    check(f.runs("off"), "a DIGU bookmark storing med leaves the data-mode AGC off");
    check(f.backend->currentOperatingState().agcMode == QLatin1String("slow"),
          "and the operator's remembered AGC is still the one held behind it");

    // Saving the entry again reads SliceModel::agcMode().
    check(f.slice->agcMode() == QLatin1String("off"),
          "saving the bookmark again stores off, so the stale value ages out");

    f.slice->setMode(QStringLiteral("USB"));
    check(f.runs("slow"), "leaving DIGU returns the AGC held before the recall");

    f.operate("LSB", "slow");
    recall(f.radio, f.slice, bookmark("DIGL", "fast", 65));
    check(f.runs("off"), "a DIGL bookmark storing fast leaves the data-mode AGC off");
}

void testThresholdStillRecalled()
{
    Hl2Fixture f;
    f.operate("LSB", "slow");
    recall(f.radio, f.slice, bookmark("DIGU", "med", 40));
    check(f.threshold == 40 && f.slice->agcThreshold() == 40,
          "the bookmark's AGC threshold is still recalled in DIGU");
    check(f.runs("off"), "and the threshold recall keeps the data-mode AGC off");
    f.slice->setMode(QStringLiteral("USB"));
    check(f.runs("slow"), "and the held AGC still returns on leaving");
}

void testCurrentDataModeBookmark()
{
    Hl2Fixture f;
    f.operate("LSB", "slow");
    recall(f.radio, f.slice, bookmark("DIGU", "off", 65));
    check(f.runs("off"), "a DIGU bookmark storing off recalls to off");
    f.slice->setMode(QStringLiteral("USB"));
    check(f.runs("slow"), "and leaving it returns the held AGC");
}

void testVoiceModeBookmarkKeepsItsAgc()
{
    Hl2Fixture f;
    f.operate("USB", "slow");
    recall(f.radio, f.slice, bookmark("LSB", "fast", 70));
    check(f.runs("fast") && f.threshold == 70,
          "an LSB bookmark recalls its stored AGC mode and threshold");

    // Out of a data mode: the held AGC returns first, then the bookmark's.
    f.operate("USB", "slow");
    f.slice->setMode(QStringLiteral("DIGU"));
    check(f.runs("off"), "fixture: DIGU runs AGC off");
    recall(f.radio, f.slice, bookmark("LSB", "fast", 70));
    check(f.runs("fast"), "an LSB bookmark recalled from DIGU gets its stored AGC");
}

void testOperatorChoiceInDataMode()
{
    Hl2Fixture f;
    f.operate("LSB", "slow");
    f.slice->setMode(QStringLiteral("DIGU"));
    f.slice->setAgcMode(QStringLiteral("fast"));
    check(f.runs("fast"), "the operator's own AGC choice in DIGU is applied");

    // A recall while that choice is live does not touch it.
    recall(f.radio, f.slice, bookmark("DIGU", "med", 65));
    check(f.runs("fast"), "a DIGU recall leaves the operator's live DIGU AGC alone");
    f.slice->setMode(QStringLiteral("USB"));
    check(f.runs("fast"), "and that choice is still kept on leaving DIGU");

    // THE LIMIT: the entry holds only the mode string, so a bookmark saved
    // with that choice is not told apart from a stale one.
    f.operate("LSB", "slow");
    recall(f.radio, f.slice, bookmark("DIGU", "fast", 65));
    check(f.runs("off"),
          "limit: a DIGU bookmark saved with a chosen AGC recalls to off, not to it");
    f.slice->setAgcMode(QStringLiteral("fast"));
    check(f.runs("fast"), "and the operator can still select it after the recall");
}

// Every backend but the HL2 takes the bookmark's AGC as before.
class FlexShapedBackend final : public IRadioBackend
{
public:
    FlexBackend flex;
    QStringList wire;
    QStringList pairedModes;

    FlexShapedBackend()
    {
        flex.setSliceCommandSink([this](const QString& c) { wire.append(c); });
    }

    RadioCapabilities capabilities() const override { return {}; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    bool isConnected() const override { return true; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString& mode, int) override { pairedModes.append(mode); }
    void requestSliceAgc(int s, const SliceAgcRequest& r) override
    {
        flex.requestSliceAgc(s, r);
        IRadioBackend::requestSliceAgc(s, r);
    }
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

void testOtherBackendsUnchanged()
{
    RadioModel radio;
    auto owned = std::make_unique<FlexShapedBackend>();
    FlexShapedBackend* backend = owned.get();
    radio.setBackendForTest(std::move(owned), QStringLiteral("bandstack-agc-test"));
    const QString pan = QStringLiteral("bandstack-agc/pan0");
    emit backend->panCenterBandwidthChanged(pan, 14.074, 0.1);
    SliceDelta delta;
    delta.panId = pan;
    delta.frequency = 14.074;
    delta.mode = QStringLiteral("DIGU");
    delta.agcMode = QStringLiteral("off");
    delta.agcThreshold = 65;
    delta.inUse = true;
    emit backend->sliceChanged(0, delta);
    SliceModel* slice = radio.slice(0);
    if (!slice) {
        std::fprintf(stderr, "FATAL: the non-HL2 slice was not materialised\n");
        std::exit(2);
    }

    recall(radio, slice, bookmark("DIGU", "med", 70));
    const QStringList expected{
        QStringLiteral("slice set 0 agc_mode=med"),
        QStringLiteral("slice set 0 agc_threshold=70"),
    };
    check(backend->wire == expected,
          "flex: a DIGU bookmark still sends its stored AGC mode and threshold");
    check(!backend->pairedModes.isEmpty() && backend->pairedModes.first() == QLatin1String("med"),
          "base seam: a backend with no data-mode AGC default still gets the stored mode");
    check(slice->agcMode() == QLatin1String("med"),
          "and the slice shows the bookmark's AGC there");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-bandstack-digital-agc"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");

    testStaleDataModeBookmark();
    testThresholdStillRecalled();
    testCurrentDataModeBookmark();
    testVoiceModeBookmarkKeepsItsAgc();
    testOperatorChoiceInDataMode();
    testOtherBackendsUnchanged();

    if (g_failed == 0) {
        std::printf("hl2_bandstack_digital_agc_test: all checks passed\n");
    }
    return g_failed == 0 ? 0 : 1;
}
