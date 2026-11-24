// AetherGPS install state shared across windows: the radio's one
// AetherGpsSettle, AetherGpsTools::busy() following it and every
// WaveformInstaller, and the process-wide upload-activity signal that lets a
// window re-enable after another window's upload ends. Injected waveform
// status and a RadioModel with no backend: no sockets, radio or TX.
#include "core/AetherGpsPackage.h"
#include "core/WaveformInstaller.h"
#include "gui/AetherGpsTools.h"
#include "models/FlexWaveformModel.h"
#include "models/RadioModel.h"

#include <QPointer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest>

namespace AetherSDR {
class WaveformInstallerTestAccess {
public:
    // An upload in flight, as after the radio granted the transfer. The emit
    // stands in for beginInstall()'s, which needs a connected session to
    // reach; only the end-of-upload emits are pinned here.
    static void begin(WaveformInstaller& installer)
    {
        installer.m_activeGeneration = installer.m_uploadState.begin(4);
        emit WaveformInstallActivity::instance()->changed();
    }
};
}  // namespace AetherSDR

using namespace AetherSDR;

namespace {

void list(FlexWaveformModel& waveforms, bool withGps)
{
    WaveformStatusDelta d;
    d.kind = WaveformStatusDelta::Kind::InstalledList;
    d.installed.append({QStringLiteral("FreeDV"), QStringLiteral("1.0")});
    if (withGps) {
        d.installed.append({QString::fromLatin1(kAetherGpsWaveformName), QStringLiteral("0.1.1")});
    }
    waveforms.applyStatus(d);
}

}  // namespace

class AetherGpsToolsTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // Both windows reach the same instance, so an install accepted in one
    // holds the other's install control until the radio lists it.
    void settleIsOnePerRadio()
    {
        FlexWaveformModel waveforms;
        AetherGpsSettle* a = AetherGpsSettle::of(waveforms);
        QCOMPARE(AetherGpsSettle::of(waveforms), a);
        FlexWaveformModel other;
        QVERIFY(AetherGpsSettle::of(other) != a);
    }

    void installSettlesWhenListed()
    {
        FlexWaveformModel waveforms;
        list(waveforms, false);
        AetherGpsSettle* settle = AetherGpsSettle::of(waveforms);
        QSignalSpy changed(settle, &AetherGpsSettle::changed);
        settle->start(true);
        QCOMPARE(settle->state(), AetherGpsSettle::State::Install);
        list(waveforms, false);   // an unrelated list update
        QVERIFY(settle->active());
        list(waveforms, true);
        QVERIFY(!settle->active());
        QCOMPARE(changed.size(), 2);
    }

    void removalSettlesWhenDropped()
    {
        FlexWaveformModel waveforms;
        list(waveforms, true);
        AetherGpsSettle* settle = AetherGpsSettle::of(waveforms);
        settle->start(false);
        QCOMPARE(settle->state(), AetherGpsSettle::State::Removal);
        list(waveforms, false);
        QVERIFY(!settle->active());
    }

    // A refused removal (or install) ends the wait at once.
    void refusalEndsIt()
    {
        FlexWaveformModel waveforms;
        list(waveforms, true);
        AetherGpsSettle* settle = AetherGpsSettle::of(waveforms);
        settle->start(false);
        QVERIFY(settle->active());
        settle->cancel();
        QVERIFY(!settle->active());
    }

    void nothingToWaitForWhenListAgrees()
    {
        FlexWaveformModel waveforms;
        list(waveforms, true);
        AetherGpsSettle* settle = AetherGpsSettle::of(waveforms);
        QSignalSpy changed(settle, &AetherGpsSettle::changed);
        settle->start(true);
        QVERIFY(!settle->active());
        QCOMPARE(changed.size(), 0);
    }

    // The session ended: no list is coming, so nothing stays disabled.
    void sessionEndClearsIt()
    {
        FlexWaveformModel waveforms;
        AetherGpsSettle* settle = AetherGpsSettle::of(waveforms);
        settle->start(true);
        QVERIFY(settle->active());
        waveforms.clear();
        QVERIFY(!settle->active());
    }

    // An install the AetherGPS window's tools didn't start (the Waveforms
    // window's) still makes them busy, and they say so.
    void toolsFollowTheSharedSettle()
    {
        RadioModel radio;
        AetherGpsTools tools(&radio);
        QSignalSpy changed(&tools, &AetherGpsTools::changed);
        QVERIFY(!tools.busy());
        AetherGpsTools other(&radio);
        other.awaitListing(true);
        QVERIFY(tools.busy());
        QVERIFY(changed.size() >= 1);
        list(radio.flexWaveformModel(), true);
        QVERIFY(!tools.busy());
    }

    // Another window's upload ending is announced to every window, after
    // anyInstalling() has gone false, including when it fails or is
    // cancelled and the radio's list never changes.
    void uploadEndIsAnnounced()
    {
        WaveformInstaller installer(nullptr);
        QList<bool> seen;
        QObject context;
        connect(WaveformInstallActivity::instance(), &WaveformInstallActivity::changed, &context,
                [&seen] { seen.append(WaveformInstaller::anyInstalling()); });
        WaveformInstallerTestAccess::begin(installer);
        QVERIFY(WaveformInstaller::anyInstalling());
        installer.cancel();
        QCOMPARE(seen, (QList<bool>{true, false}));
    }

    void destroyedUploadIsAnnounced()
    {
        auto* installer = new WaveformInstaller(nullptr);
        WaveformInstallerTestAccess::begin(*installer);
        QSignalSpy activity(WaveformInstallActivity::instance(), &WaveformInstallActivity::changed);
        delete installer;
        QCOMPARE(activity.size(), 1);
        QVERIFY(!WaveformInstaller::anyInstalling());
    }

    // The AetherGPS window's tools re-check busy() when another window's
    // upload ends.
    void toolsFollowEveryUpload()
    {
        RadioModel radio;
        AetherGpsTools tools(&radio);
        WaveformInstaller installer(nullptr);
        WaveformInstallerTestAccess::begin(installer);
        QVERIFY(tools.busy());
        QSignalSpy changed(&tools, &AetherGpsTools::changed);
        installer.cancel();
        QVERIFY(changed.size() >= 1);
        QVERIFY(!tools.busy());
    }
};

QTEST_MAIN(AetherGpsToolsTest)
#include "aethergps_tools_test.moc"
