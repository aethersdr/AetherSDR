// Socket-free production widgets and SliceModel. The injected backend supplies
// capability data only; this is not a simulator peer or a hardware test.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/SliceDelta.h"
#include "gui/RxApplet.h"
#include "gui/FilterPassbandWidget.h"
#include "gui/ReceiveCaptureAction.h"
#include "gui/VfoWidget.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QImage>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>

using namespace AetherSDR;

namespace AetherSDR {
struct RadioModelWakeTestAccess {
    static void identity(RadioModel& radio, const QString& serial)
    { radio.m_lastInfo.serial = serial; }
};
}

namespace {
class FilterBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    int captureRequests = 0;
    QString lastCapturePan;
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return true; }
    ReceiveControlPolicy receiveControlPolicy() const override { return ReceiveControlPolicy::Confirmed; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    bool recenterReceiveCapture(const QString& pan) override
    { ++captureRequests; lastCapturePan = pan; return true; }
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

void setMode(SliceModel& slice, const QString& mode)
{
    SliceDelta delta;
    delta.mode = mode;
    delta.filterLow = -8000;
    delta.filterHigh = 8000;
    slice.applyChanges(delta);
}

QPushButton* button(QWidget& widget, const QString& text)
{
    for (QPushButton* candidate : widget.findChildren<QPushButton*>()) {
        if (candidate->text().compare(text, Qt::CaseInsensitive) == 0) {
            return candidate;
        }
    }
    return nullptr;
}
} // namespace

class FmFilterControlsTest : public QObject {
    Q_OBJECT
private slots:
    void captureActionUsesLiveCapabilityAndModelIntent()
    {
        RadioModel model;
        auto backend = std::make_unique<FilterBackend>();
        FilterBackend* source = backend.get();
        model.setBackendForTest(std::move(backend), QStringLiteral("test"));
        emit source->panCenterBandwidthChanged(QStringLiteral("0xe1000000"), 100.0, 1.0);
        ReceiveCaptureAction action(model, [] { return QStringLiteral("0xe1000000"); }, &model);
        QVERIFY(!action.isEnabled());
        QVERIFY(!action.statusTip().isEmpty());
        action.trigger();
        QCOMPARE(source->captureRequests, 0);
        source->caps.receiveCapturePlacement = ReceiveCapturePlacement{48'000};
        // The injection seam wires receiver state only; deliver the same
        // capability event that setupBackend publishes in a real session.
        emit model.capabilitiesChanged(true, source->caps);
        QVERIFY(action.isEnabled());
        action.trigger();
        QCOMPARE(source->captureRequests, 1);
        QCOMPARE(source->lastCapturePan, QStringLiteral("0xe1000000"));
        source->caps.receiveCapturePlacement.reset();
        emit model.capabilitiesChanged(true, source->caps);
        QVERIFY(!action.isEnabled());
        QVERIFY(!model.requestReceiveCaptureRecenter(QStringLiteral("0xe1000000")));
        QCOMPARE(source->captureRequests, 1);
    }

    void squelchUsesDeclaredModesAndAbsoluteAutoThreshold()
    {
        RadioModel model;
        auto backend = std::make_unique<FilterBackend>();
        backend->caps.receiveSquelchModel = ReceiveSquelchModel{
            {QStringLiteral("FM"), QStringLiteral("FMN")}, -120, 1.2, QStringLiteral("dBFS/bin")};
        FilterBackend* source = backend.get();
        model.setBackendForTest(std::move(backend), QStringLiteral("test"));
        RadioModelWakeTestAccess::identity(model, QStringLiteral("sql-controls"));
        const RadioSettingsScope scope = model.settingsScope();
        QVERIFY(scope.setFeature(QStringLiteral("ReceiveSquelchIntent-3"), 1,
            {{QStringLiteral("manualLevel"), 44}, {QStringLiteral("autoEnabled"), false}}));
        SliceDelta initial;
        initial.mode = QStringLiteral("FMN"); initial.frequency = 100.3;
        initial.filterLow = -8000; initial.filterHigh = 8000;
        initial.squelchOn = false; initial.squelchLevel = 44;
        emit source->sliceChanged(3, initial);
        QVERIFY(model.slice(3));
        SliceModel& slice = *model.slice(3);
        slice.applyChanges(initial); slice.setManualSquelchLevel(44);
        RxApplet rx;
        VfoWidget vfo;
        rx.setSlice(&slice); vfo.setSlice(&slice);
        rx.setRadioModel(&model); vfo.setRadioModel(&model);
        QPushButton* rxSql = button(rx, QStringLiteral("SQL"));
        QPushButton* vfoSql = button(vfo, QStringLiteral("SQL"));
        QVERIFY(rxSql && vfoSql);
        QVERIFY(rxSql->isEnabled() && vfoSql->isEnabled());
        QSignalSpy intents(&slice, &SliceModel::squelchCommandIssued);
        rxSql->click();
        QCOMPARE(intents.count(), 1);
        QCOMPARE(intents.last().at(1).toInt(), 44);
        rxSql->click();
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Auto);
        QCOMPARE(intents.count(), 2);
        QCOMPARE(intents.last().at(1).toInt(), 44); // never the 10 dB margin
        QVERIFY(!slice.squelchOn()); // waits for accepted state
        initial.squelchOn = true;
        slice.applyChanges(initial);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Auto);
        rx.setSlice(nullptr); // flush this receiver's coalesced preference
        QVERIFY(scope.featureExact(QStringLiteral("ReceiveSquelchIntent-3"))
            .value(QStringLiteral("autoEnabled")).toBool());
        emit source->sliceChanged(5, initial);
        QVERIFY(model.slice(5));
        rx.setSlice(model.slice(5));
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Manual);
        rx.setSlice(&slice);
        QCOMPARE(rx.sqlMode(), RxApplet::SqlMode::Auto);
        QCOMPARE(rx.sqlManualLevel(), 44);
        setMode(slice, QStringLiteral("WFM"));
        QVERIFY(!rxSql->isEnabled() && !vfoSql->isEnabled());
        QVERIFY(!rxSql->accessibleDescription().isEmpty());
        QVERIFY(!vfoSql->accessibleDescription().isEmpty());
        const int before = intents.count();
        rx.cycleSqlModeExternal();
        QCOMPARE(intents.count(), before);
        setMode(slice, QStringLiteral("FM"));
        QVERIFY(rxSql->isEnabled() && vfoSql->isEnabled());
        initial.mode = QStringLiteral("FM"); initial.squelchOn = false;
        slice.applyChanges(initial);
        rx.setSlice(nullptr);
        QVERIFY(!scope.featureExact(QStringLiteral("ReceiveSquelchIntent-3"))
            .value(QStringLiteral("autoEnabled")).toBool());
    }

    void declaredFmFiltersReachBothWidgets_data()
    {
        QTest::addColumn<QString>("mode");
        for (const char* mode : {"FM", "FMN", "NFM"}) {
            QTest::newRow(mode) << QString::fromLatin1(mode);
        }
    }

    void declaredFmFiltersReachBothWidgets()
    {
        QFETCH(QString, mode);
        RadioModel model;
        auto backend = std::make_unique<FilterBackend>();
        backend->caps.receiveFilterControl = ReceiveFilterControl{
            SliceFrequencyControl::Authority::Engine,
            {{QStringLiteral("FM"), -21600, -1, 1, 21600, 2, 43200},
             {QStringLiteral("FMN"), -21600, -1, 1, 21600, 2, 43200}}};
        model.setBackendForTest(std::move(backend), QStringLiteral("test"));
        SliceModel slice(3);
        setMode(slice, mode);
        RxApplet rx;
        VfoWidget vfo;
        rx.setSlice(&slice);
        vfo.setSlice(&slice);
        // Capability binding after slice attachment exercises the lazy path.
        rx.setRadioModel(&model);
        vfo.setRadioModel(&model);
        FilterPassbandWidget* passband = rx.findChild<FilterPassbandWidget*>();
        QVERIFY(passband);
        QVERIFY(passband->isEnabled());
        QSignalSpy intents(&slice, &SliceModel::filterCommandIssued);
        for (QWidget* surface : {static_cast<QWidget*>(&rx), static_cast<QWidget*>(&vfo)}) {
            QPushButton* preset = button(*surface, QStringLiteral("8K"));
            QVERIFY(preset);
            QVERIFY(!preset->isHidden());
            QVERIFY(preset->isEnabled());
            intents.clear();
            preset->click();
            QCOMPARE(intents.count(), 1);
            QCOMPARE(intents.at(0).at(0).toInt(), -4000);
            QCOMPARE(intents.at(0).at(1).toInt(), 4000);
            setMode(slice, mode);
        }
        passband->resize(200, 80);
        const QImage enabledPassband = passband->grab().toImage();
        setMode(slice, QStringLiteral("WFM"));
        QVERIFY(!passband->isEnabled());
        QVERIFY(!passband->accessibleDescription().isEmpty());
        passband->resize(200, 80);
        QVERIFY(enabledPassband != passband->grab().toImage());
        for (QWidget* surface : {static_cast<QWidget*>(&rx), static_cast<QWidget*>(&vfo)}) {
            QVERIFY(!button(*surface, QStringLiteral("1.8K")));
            QVERIFY(!button(*surface, QStringLiteral("8K")));
            QPushButton* unavailable = button(*surface, QStringLiteral("Filter unavailable"));
            QVERIFY(unavailable);
            QVERIFY(!unavailable->isHidden());
            QVERIFY(!unavailable->isEnabled());
            QVERIFY(!unavailable->accessibleDescription().isEmpty());
        }
        setMode(slice, mode);
        QVERIFY(passband->isEnabled());
        QVERIFY(button(rx, QStringLiteral("8K")));
        QVERIFY(button(vfo, QStringLiteral("8K")));
        // A stale saved SSB-shaped filter must neither return nor erase the
        // usable default ladder during the filter-highlight refresh.
        AppSettings::instance().setValue(QStringLiteral("FilterPresets_%1").arg(mode),
                                         QStringLiteral("95:8000"));
        setMode(slice, QStringLiteral("WFM"));
        setMode(slice, mode);
        QVERIFY(button(rx, QStringLiteral("8K")));
        QVERIFY(button(vfo, QStringLiteral("8K")));
        AppSettings::instance().remove(QStringLiteral("FilterPresets_%1").arg(mode));
    }

    void fixedFmDoesNotAcquireAnAdjustableLadder()
    {
        RadioModel model;
        auto backend = std::make_unique<FilterBackend>();
        backend->caps.receiveFilterControl = ReceiveFilterControl{
            SliceFrequencyControl::Authority::Radio,
            {{QStringLiteral("USB"), 0, 11990, 10, 12000, 10, 12000}}};
        model.setBackendForTest(std::move(backend), QStringLiteral("test"));
        SliceModel slice(0);
        setMode(slice, QStringLiteral("FM"));
        RxApplet rx;
        VfoWidget vfo;
        rx.setRadioModel(&model);
        vfo.setRadioModel(&model);
        rx.setSlice(&slice);
        vfo.setSlice(&slice);
        QVERIFY(!button(rx, QStringLiteral("8K")));
        QVERIFY(!button(vfo, QStringLiteral("8K")));
        // The headless record deliberately omits CW; desktop CW remains usable.
        setMode(slice, QStringLiteral("CW"));
        QVERIFY(button(rx, QStringLiteral("500")));
        QVERIFY(button(vfo, QStringLiteral("500")));
    }

    void fixedRadioLadderStillWins()
    {
        RadioModel model;
        model.setBackendForTest(std::make_unique<FilterBackend>(), QStringLiteral("test"));
        SliceModel slice(0);
        setMode(slice, QStringLiteral("FM"));
        RxApplet rx;
        VfoWidget vfo;
        rx.setRadioModel(&model);
        vfo.setRadioModel(&model);
        rx.setSlice(&slice);
        vfo.setSlice(&slice);
        const QList<int> fixed{16000, 12000, 8000};
        rx.setRadioFilterWidths(fixed);
        vfo.setRadioFilterWidths(fixed);
        for (QWidget* surface : {static_cast<QWidget*>(&rx), static_cast<QWidget*>(&vfo)}) {
            QVERIFY(button(*surface, QStringLiteral("8K")));
            QVERIFY(!button(*surface, QStringLiteral("6K")));
            QVERIFY(!button(*surface, QStringLiteral("Filter unavailable")));
        }
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("fm-filter-controls"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    FmFilterControlsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "fm_filter_controls_test.moc"
