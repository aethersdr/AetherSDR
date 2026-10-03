// RIT / XIT cross the IRadioBackend seam per slice (#6105), as FlexLib models
// them (Slice.RITOn/RITFreq/XITOn/XITFreq). Socket-free: an injected backend
// records each RIT/XIT verb with the slice id RadioModel passes, two slices are
// materialised the way a seam backend's first sliceChanged does, and the Flex
// "slice set <n> rit_*/xit_*" command text is read off SliceModel::commandReady.

#include "TestSettingsProfile.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>
#include <memory>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool ok, const char* message)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}

class RecordingBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    QStringList calls;
    bool clampRit = false;
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const AetherSDR::TxCoordinator::Operation&,
                   const AetherSDR::TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    void setSliceRitEnabled(int id, bool on) override { log("ritOn", id, on); }
    void setSliceRitOffset(int id, int hz) override
    {
        log("ritHz", id, hz);
        if (clampRit && hz > 9999) {
            // A backend that answers synchronously, as HL2's clamp does.
            SliceDelta d;
            d.ritFreq = 9999;
            emit sliceChanged(id, d);
        }
    }
    void setSliceXitEnabled(int id, bool on) override { log("xitOn", id, on); }
    void setSliceXitOffset(int id, int hz) override { log("xitHz", id, hz); }
    void log(const char* what, int id, int v)
    {
        calls << QStringLiteral("%1 %2 %3").arg(QLatin1String(what)).arg(id).arg(v);
    }
};

void materialise(RecordingBackend* backend, int id, bool tx)
{
    SliceDelta d;
    d.frequency = 14.074 + id;
    d.mode = QStringLiteral("USB");
    d.txSlice = tx;
    emit backend->sliceChanged(id, d);
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("slice-rit-xit-seam-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");

    RadioModel radio;
    auto owned = std::make_unique<RecordingBackend>();
    RecordingBackend* backend = owned.get();
    backend->caps.family = QStringLiteral("seam-test");
    radio.setBackendForTest(std::move(owned), QStringLiteral("seam-test"));
    materialise(backend, 0, true);
    materialise(backend, 1, false);
    SliceModel* a = radio.slice(0);
    SliceModel* b = radio.slice(1);
    check(a && b, "two seam slices materialised");
    if (!a || !b) {
        return 1;
    }

    QStringList wire;
    QObject::connect(b, &SliceModel::commandReady, &radio,
                     [&](const QString& cmd) { wire << cmd; });

    // ---- RIT on the NON-transmit slice carries that slice's id ----
    backend->calls.clear();
    b->setRit(true, 500);
    check(backend->calls == QStringList({"ritOn 1 1", "ritHz 1 500"}),
          "RIT on slice 1 reaches the seam as slice 1, enable then offset");
    check(wire == QStringList({"slice set 1 rit_on=1 rit_freq=500"}),
          "Flex command text for RIT is unchanged");

    // ---- XIT on the transmit slice carries its own id ----
    backend->calls.clear();
    a->setXit(true, -300);
    check(backend->calls == QStringList({"xitOn 0 1", "xitHz 0 -300"}),
          "XIT on slice 0 reaches the seam as slice 0, enable then offset");

    wire.clear();
    b->setXit(false, 0);
    check(wire == QStringList({"slice set 1 xit_on=0 xit_freq=0"}),
          "Flex command text for XIT is unchanged");

    // ---- readback: an unchanged echo is silent, a changed one corrects ----
    int ritSignals = 0;
    int lastHz = 0;
    QObject::connect(b, &SliceModel::ritChanged, &radio, [&](bool, int hz) {
        ++ritSignals;
        lastHz = hz;
    });
    SliceDelta same;
    same.ritOn = true;
    same.ritFreq = 500;
    b->applyChanges(same);
    check(ritSignals == 0, "an echo of the held RIT emits nothing");
    SliceDelta clamped;
    clamped.ritFreq = 9999;
    b->applyChanges(clamped);
    check(ritSignals == 1 && lastHz == 9999 && b->ritFreq() == 9999 && b->ritOn(),
          "a corrected RIT offset is adopted and announced once");

    // ---- a synchronous backend correction is the value setRit() announces last ----
    backend->clampRit = true;
    b->setRit(true, 20000);
    check(lastHz == 9999 && b->ritFreq() == 9999,
          "setRit() ends on the backend's clamped offset, not the requested one");

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
