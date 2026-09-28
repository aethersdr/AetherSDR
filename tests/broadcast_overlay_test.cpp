// Socket-free current-reception presentation. No receiver, cluster, audio or RF.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/SliceDelta.h"
#include "gui/WfmBroadcastOverlay.h"
#include "gui/WfmBroadcastTicker.h"
#include "gui/WfmPresentationSettings.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QtTest>

using namespace AetherSDR;
namespace {
class Backend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{true};
    int commands{0};
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    ReceiveControlPolicy receiveControlPolicy() const override { return ReceiveControlPolicy::Confirmed; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override { ++commands; }
    void setSliceMode(int, const QString&) override { ++commands; }
    void setSliceFilter(int, int, int) override { ++commands; }
    void setSliceAgc(int, const QString&, int) override { ++commands; }
    void setPanCenter(const QString&, double, PanCenterIntent) override { ++commands; }
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override { ++commands; }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override { ++commands; }
};
HdFmReception reception(int program = 0, qint64 frequency = 100300000)
{
    HdFmReception value;
    value.valid = true;
    value.sessionId = 11;
    value.receiverEpoch = 22;
    value.revision = 33;
    value.frequencyHz = frequency;
    value.selectedProgram = program;
    value.observationSequence = 1;
    value.synced = value.audioValid = true;
    value.services = {{0, QStringLiteral("Main"), true}, {1, QStringLiteral("Second"), true}};
    value.stationName = QStringLiteral("Station");
    value.title = QStringLiteral("First song");
    value.artist = QStringLiteral("Artist");
    return value;
}
SliceDelta hdSlice(int program = 0, qint64 frequency = 100300000)
{
    SliceDelta value;
    value.mode = QStringLiteral("WFM");
    value.panId = QStringLiteral("test-pan");
    value.frequency = frequency / 1.0e6;
    value.inCapture = true;
    value.wfmAudioMode = WfmAudioMode::HdStereo;
    value.hdProgram = program;
    value.hdFmReception = reception(program, frequency);
    return value;
}
Backend* attach(RadioModel& model)
{
    auto backend = std::make_unique<Backend>();
    backend->caps.broadcastFmReceive = BroadcastFmReceive{{50, 75}, true, true, true};
    Backend* source = backend.get();
    model.setBackendForTest(std::move(backend), QStringLiteral("test"));
    emit source->sliceChanged(3, hdSlice());
    return source;
}
}
class BroadcastOverlayTest : public QObject {
    Q_OBJECT
private slots:
    void init() { AppSettings::instance().remove(QStringLiteral("WfmApplet")); }
    void tickerPagesUseWordBoundariesAndReadableEndpointPauses()
    {
        WfmBroadcastOverlayRecord record{3, 11, 22, 33, 100300000, 0,
            QStringLiteral("Station"), QStringLiteral("One two three four five six seven eight nine ten eleven twelve"), {}};
        const WfmBroadcastOverlayRecord original = record;
        QFont font; font.setPixelSize(12);
        const QFontMetrics metrics(font);
        const int width = metrics.horizontalAdvance(QStringLiteral("One two three"));
        WfmBroadcastTicker ticker;
        QVERIFY(ticker.setContent(record));
        QVERIFY(ticker.layout(font, width));
        QVERIFY(ticker.pages().size() > 3);
        QCOMPARE(ticker.pages().join(QLatin1Char(' ')), record.displayText().simplified());
        for (const QString& page : ticker.pages()) { QVERIFY(metrics.horizontalAdvance(page) <= width); }
        const int stableWidth = ticker.labelWidth();
        QVERIFY(!ticker.advance(100, true));
        QVERIFY(!ticker.advance(5099, true));
        QVERIFY(ticker.advance(5100, true));
        QCOMPARE(ticker.pageIndex(), 1);
        QVERIFY(!ticker.advance(8099, true));
        QVERIFY(ticker.advance(8100, true));
        qint64 now = 8100;
        while (ticker.pageIndex() < ticker.pages().size() - 1) {
            now += 3000;
            QVERIFY(ticker.advance(now, true));
        }
        QVERIFY(!ticker.advance(now + 4999, true));
        QVERIFY(ticker.advance(now + 5000, true));
        QCOMPARE(ticker.pageIndex(), 0);
        QCOMPARE(ticker.labelWidth(), stableWidth);
        QVERIFY(record == original); // Paging never mutates/reinserts metadata.
    }
    void tickerIgnoresControlRevisionsButResetsContentServiceAndGeneration()
    {
        WfmBroadcastOverlayRecord record{3, 11, 22, 33, 100300000, 0,
            QStringLiteral("Station"), QStringLiteral("A longer station announcement with many words for paging"), {}};
        QFont font; font.setPixelSize(12);
        WfmBroadcastTicker ticker;
        ticker.setContent(record); ticker.layout(font, 100);
        ticker.advance(0, true); QVERIFY(ticker.advance(5000, true));
        const int page = ticker.pageIndex();
        ++record.revision;
        QVERIFY(!ticker.setContent(record));
        QVERIFY(!ticker.layout(font, 100));
        QCOMPARE(ticker.pageIndex(), page);
        QVERIFY(!ticker.advance(6000, false));
        QVERIFY(!ticker.advance(90000, true)); // Hidden time is not catch-up work.
        QCOMPARE(ticker.pageIndex(), page);
        QVERIFY(!ticker.advance(92999, true));
        QVERIFY(ticker.advance(93000, true));
        record.title = QStringLiteral("Replacement text at the same frequency");
        QVERIFY(ticker.setContent(record)); ticker.layout(font, 100);
        QCOMPARE(ticker.pageIndex(), 0);
        QVERIFY(!ticker.fullText().contains(QStringLiteral("announcement")));
        for (int change = 0; change < 4; ++change) {
            ticker.advance(0, true); QVERIFY(ticker.advance(5000, true));
            if (change == 0) { ++record.program; }
            if (change == 1) { ++record.receiverEpoch; }
            if (change == 2) { ++record.sessionId; }
            if (change == 3) { ++record.frequencyHz; }
            QVERIFY(ticker.setContent(record)); ticker.layout(font, 100);
            QCOMPARE(ticker.pageIndex(), 0);
        }
        record.title.clear(); record.stationName.clear(); record.artist.clear();
        QVERIFY(ticker.setContent(record)); ticker.layout(font, 400);
        QCOMPARE(ticker.pages().size(), 1);
        QVERIFY(!ticker.advance(0, true)); QVERIFY(!ticker.advance(100000, true));
        QCOMPARE(ticker.pageText(), record.displayText());
        ticker = {}; // Overlay off/loss removes presentation state entirely.
        QVERIFY(ticker.fullText().isEmpty()); QVERIFY(ticker.pages().isEmpty());
        QVERIFY(!ticker.advance(200000, true));
        ticker.setContent(record); ticker.layout(font, 400);
        QCOMPARE(ticker.pageIndex(), 0);
    }
    void tickerLongWordsPreserveUnicodeGraphemesAndAlwaysAdvance()
    {
        QFont font; font.setPixelSize(12);
        const QString cluster = QString::fromUtf8("👩🏽‍🚀");
        const QString combining = QString::fromUtf8("é");
        WfmBroadcastOverlayRecord record{3, 11, 22, 33, 100300000, 0,
            {}, cluster.repeated(20) + combining.repeated(20), {}};
        WfmBroadcastTicker ticker;
        ticker.setContent(record);
        const int width = std::max(QFontMetrics(font).horizontalAdvance(cluster),
                                   QFontMetrics(font).horizontalAdvance(QStringLiteral("HD Radio")));
        ticker.layout(font, width);
        QVERIFY(ticker.pages().size() > 2);
        QString reconstructed;
        for (const QString& page : ticker.pages()) { QVERIFY(!page.isEmpty()); reconstructed += page; }
        QString expected = ticker.fullText(); expected.remove(QLatin1Char(' '));
        reconstructed.remove(QLatin1Char(' '));
        QCOMPARE(reconstructed, expected);
        for (const QString& page : ticker.pages()) {
            QString remainder = page;
            remainder.remove(cluster); remainder.remove(combining);
            QVERIFY(!remainder.contains(QChar(0x200d))); // No partial joiner sequence.
            QVERIFY(!remainder.contains(QChar(0x0301))); // No detached combining mark.
            for (const QChar ch : remainder) { QVERIFY(!ch.isSurrogate()); }
        }
        ticker.layout(font, 1); // Even an oversized glyph beside a space must progress.
        reconstructed.clear();
        for (const QString& page : ticker.pages()) { QVERIFY(!page.isEmpty()); reconstructed += page; }
        reconstructed.remove(QLatin1Char(' '));
        QCOMPARE(reconstructed, expected);
    }
    void freshDefaultAndExplicitFalseHaveOnePersistentOwner()
    {
        auto& settings = WfmPresentationSettings::instance();
        QVERIFY(settings.broadcastOverlayEnabled());
        AppSettings::instance().setValue(QStringLiteral("WfmApplet"),
            QStringLiteral(R"({"other":17,"ui":{"retained":"yes","showLockScope":false}})"));
        QSignalSpy changed(&settings, &WfmPresentationSettings::overlayEnabledChanged);
        settings.setBroadcastOverlayEnabled(false);
        QCOMPARE(changed.count(), 1);
        QVERIFY(!settings.broadcastOverlayEnabled());
        QVERIFY(!settings.showLockScope());
        settings.setAppletOptions(true, true);
        AppSettings::instance().load();
        QVERIFY(!settings.broadcastOverlayEnabled());
        QVERIFY(settings.showLockScope());
        QVERIFY(settings.showDiagnostics());
        const QJsonObject document = QJsonDocument::fromJson(AppSettings::instance()
            .value(QStringLiteral("WfmApplet")).toString().toUtf8()).object();
        QCOMPARE(document.value(QStringLiteral("other")).toInt(), 17);
        QCOMPARE(document.value(QStringLiteral("ui")).toObject().value(QStringLiteral("retained")).toString(), QStringLiteral("yes"));
        settings.setBroadcastOverlayEnabled(false);
        QCOMPARE(changed.count(), 1);
    }
    void sameLocalEntryReplacesSongWithoutSpotOrCommandSideEffects()
    {
        RadioModel model;
        Backend* source = attach(model);
        WfmBroadcastOverlay overlay;
        overlay.bind(&model, model.slice(3)->panId());
        QCOMPARE(overlay.records().size(), 1);
        QCOMPARE(overlay.records().first().sliceId, 3);
        QVERIFY(overlay.records().first().displayText().startsWith(QStringLiteral("HD Radio · HD1")));
        QSignalSpy changed(&overlay, &WfmBroadcastOverlay::overlaysChanged);
        for (int i = 0; i < 30; ++i) {
            HdFmReception value = reception();
            value.observationSequence = i + 2;
            value.title = QStringLiteral("Song %1 <not markup>").arg(i);
            SliceDelta delta; delta.hdFmReception = value;
            emit source->sliceChanged(3, delta);
            QCOMPARE(overlay.records().size(), 1);
            QCOMPARE(overlay.records().first().title, value.title);
        }
        QCOMPARE(changed.count(), 30);
        QCOMPARE(source->commands, 0);
        QVERIFY(model.spotModel().spots().isEmpty());
        WfmPresentationSettings::instance().setBroadcastOverlayEnabled(false);
        QVERIFY(overlay.records().isEmpty());
        QVERIFY(model.slice(3)->hdFmReception().valid);
        QCOMPARE(model.slice(3)->hdFmReception().title, QStringLiteral("Song 29 <not markup>"));
        WfmPresentationSettings::instance().setBroadcastOverlayEnabled(true);
        QCOMPARE(overlay.records().size(), 1);
        QCOMPARE(overlay.records().first().title, QStringLiteral("Song 29 <not markup>"));
        QCOMPARE(source->commands, 0);
    }
    void acceptedIdentityAndPanOwnershipClearOldPresentation()
    {
        RadioModel model;
        Backend* source = attach(model);
        WfmBroadcastOverlay overlay;
        const QString pan = model.slice(3)->panId();
        overlay.bind(&model, QStringLiteral("foreign-pan"));
        QVERIFY(overlay.records().isEmpty());
        overlay.bind(&model, pan);
        QCOMPARE(overlay.records().size(), 1);
        model.slice(3)->setHdProgram(1); // intent must not clear the accepted entry
        QCOMPARE(overlay.records().first().program, 0);
        SliceDelta change;
        change.hdProgram = 1;
        emit source->sliceChanged(3, change);
        QVERIFY(overlay.records().isEmpty());
        change = {};
        change.hdFmReception = reception(1);
        emit source->sliceChanged(3, change);
        QCOMPARE(overlay.records().first().program, 1);
        change = {}; change.frequency = 101.1;
        emit source->sliceChanged(3, change);
        QVERIFY(overlay.records().isEmpty());
        change = {}; change.hdFmReception = reception(1); // old frequency is rejected
        emit source->sliceChanged(3, change);
        QVERIFY(overlay.records().isEmpty());
        change.hdFmReception = reception(1, 101100000);
        emit source->sliceChanged(3, change);
        QCOMPARE(overlay.records().size(), 1);
        change = {}; change.inCapture = false;
        emit source->sliceChanged(3, change);
        QVERIFY(overlay.records().isEmpty());
        change.inCapture = true;
        emit source->sliceChanged(3, change);
        QCOMPARE(overlay.records().size(), 1);
        change = {}; change.mode = QStringLiteral("AM");
        emit source->sliceChanged(3, change);
        QVERIFY(overlay.records().isEmpty());
    }
    void disconnectCapabilityRetirementAndRebindCannotRetainLabels()
    {
        RadioModel model;
        Backend* source = attach(model);
        WfmBroadcastOverlay overlay;
        overlay.bind(&model, model.slice(3)->panId());
        source->connected = false;
        emit model.connectionStateChanged(false);
        QVERIFY(overlay.records().isEmpty());
        source->connected = true;
        emit model.connectionStateChanged(true);
        QCOMPARE(overlay.records().size(), 1);
        source->caps.broadcastFmReceive->hdStereo = false;
        emit model.capabilitiesChanged(true, source->caps);
        QVERIFY(overlay.records().isEmpty());
        source->caps.broadcastFmReceive->hdStereo = true;
        emit model.capabilitiesChanged(true, source->caps);
        QCOMPARE(overlay.records().size(), 1);
        emit source->sliceRemoved(3);
        QVERIFY(overlay.records().isEmpty());
        emit source->sliceChanged(3, hdSlice());
        QCOMPARE(overlay.records().size(), 1);
        overlay.bind(nullptr, {});
        QVERIFY(overlay.records().isEmpty());
        emit source->sliceChanged(3, hdSlice());
        QVERIFY(overlay.records().isEmpty());
        auto other = std::make_unique<RadioModel>();
        attach(*other);
        overlay.bind(other.get(), other->slice(3)->panId());
        QCOMPARE(overlay.records().size(), 1);
        other.reset();
        QVERIFY(overlay.records().isEmpty());
    }
    void lossOfSyncClearsTextAndBoundedSlicesNeverAccumulateHistory()
    {
        RadioModel model;
        Backend* source = attach(model);
        WfmBroadcastOverlay overlay;
        overlay.bind(&model, model.slice(3)->panId());
        HdFmReception lost = reception();
        lost.synced = false;
        ++lost.observationSequence;
        SliceDelta delta; delta.hdFmReception = lost;
        emit source->sliceChanged(3, delta);
        QVERIFY(overlay.records().isEmpty());
        for (int i = 0; i < 12; ++i) { emit source->sliceChanged(i, hdSlice()); }
        QCOMPARE(overlay.records().size(), 8);
        QVERIFY(model.spotModel().spots().isEmpty());
    }
};
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("broadcast-overlay"));
    if (!profile.isValid()) { return 1; }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    BroadcastOverlayTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "broadcast_overlay_test.moc"
