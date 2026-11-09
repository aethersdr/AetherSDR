// Radio Setup -> HL2 Hardware -> Local Audio: a codec the backend SUGGESTS for
// a board its gateware names (#6806) is shown as a suggestion, and choosing
// the entry already shown stores it.
//
// Pinned here:
//   1. suggested: the line beside the Codec combo names the board and says the
//      choice is not saved, the combo's accessible description says the same,
//      and opening the page writes nothing;
//   2. choosing the shown entry again (popup, Return) sends ONE hw.set with
//      that codec, and the reseed that follows hides the line;
//   3. CONTROL: a stored codec (not suggested) shows no line, and a keyboard
//      change to another entry still sends exactly one hw.set.
//
// No socket and no radio: an injected backend that declares the hl2
// extension and answers hw.get/hw.set with the shape Hl2Backend replies with.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/IRadioBackend.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QtTest>

#include <memory>

namespace AetherSDR {
// RadioModel befriends this name; the identity is what makes the page live.
struct RadioModelWakeTestAccess {
    static void identity(RadioModel& radio, const QString& serial)
    { radio.m_lastInfo.serial = serial; }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {

class Hl2HardwareBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    int codec{2};
    bool suggested{true};
    QString board{QStringLiteral("SQUARE SDR 2")};
    QList<QVariantMap> sets;
    Hl2HardwareBackend()
    {
        caps.family = QStringLiteral("hl2");
        caps.extensionNamespaces = {QStringLiteral("hl2")};
    }
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString& ns, const QString& verb, quint64 id,
                         const QVariant& arg) override
    {
        if (ns != QLatin1String("hl2"))
            return;
        if (verb == QLatin1String("hw.set")) {
            const QVariantMap in = arg.toMap();
            sets.append(in);
            // As Hl2Backend: naming a codec ends the suggestion.
            if (in.contains(QStringLiteral("codec"))) {
                codec = in.value(QStringLiteral("codec")).toInt();
                suggested = false;
            }
            return;
        }
        if (verb == QLatin1String("hw.get") && id != 0) {
            emit extensionResult(id, QVariantMap{
                {QStringLiteral("codec"), codec},
                {QStringLiteral("ditherBit"), true},
                {QStringLiteral("randomBit"), false},
                {QStringLiteral("filterBoard"), 1},
                {QStringLiteral("n2adrHpf"), false},
                {QStringLiteral("cl1RefClock"), false},
                {QStringLiteral("atuGateware"), false},
                {QStringLiteral("speakerLevelPercent"), 100},
                {QStringLiteral("codecSuggested"), suggested},
                {QStringLiteral("detectedBoard"), board},
            });
        }
        // Other hl2 verbs (the Calibration page's) get no reply.
    }
};

struct Page {
    QComboBox* codec{nullptr};
    QLabel* suggestion{nullptr};
};

Page openPage(RadioSetupDialog& dialog)
{
    dialog.show();
    dialog.selectTab(QStringLiteral("HL2 Hardware"));
    return {dialog.findChild<QComboBox*>(QStringLiteral("hl2HwCodec")),
            dialog.findChild<QLabel*>(QStringLiteral("hl2HwCodecSuggested"))};
}

Hl2HardwareBackend* install(RadioModel& model, int codec, bool suggested)
{
    auto owned = std::make_unique<Hl2HardwareBackend>();
    Hl2HardwareBackend* backend = owned.get();
    backend->codec = codec;
    backend->suggested = suggested;
    model.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
    RadioModelWakeTestAccess::identity(model, QStringLiteral("00:1C:C0:A2:13:DD"));
    return backend;
}

const QString kSuggestion = QStringLiteral("Detected: SQUARE SDR 2 — suggested, not saved");

} // namespace

class Hl2CodecSuggestionDialogTest : public QObject {
    Q_OBJECT
private slots:

    void suggestedCodecIsShownAsASuggestionAndWritesNothing()
    {
        RadioModel model;
        Hl2HardwareBackend* backend = install(model, 2, true);
        RadioSetupDialog dialog(&model);
        const Page page = openPage(dialog);
        QVERIFY2(page.codec && page.suggestion, "codec combo and suggestion line found");
        QCOMPARE(page.codec->currentData().toInt(), 2);
        QVERIFY2(page.suggestion->isVisible(), "a suggested codec is labelled as one");
        QCOMPARE(page.suggestion->text(), kSuggestion);
        QVERIFY2(!page.suggestion->accessibleName().isEmpty(), "the line has an accessible name");
        QCOMPARE(page.codec->accessibleDescription(), kSuggestion);
        QVERIFY2(backend->sets.isEmpty(), "showing a suggestion writes nothing");
    }

    void choosingTheShownEntryStoresIt()
    {
        RadioModel model;
        Hl2HardwareBackend* backend = install(model, 2, true);
        RadioSetupDialog dialog(&model);
        const Page page = openPage(dialog);
        QVERIFY2(page.codec && page.suggestion, "codec combo and suggestion line found");
        QVERIFY2(page.codec->isEnabled(), "the page is live for a radio with an identity");

        // The operator opens the list and picks the entry already selected.
        page.codec->showPopup();
        QTRY_VERIFY(page.codec->view()->isVisible());
        QTest::keyClick(page.codec->view(), Qt::Key_Return);

        QCOMPARE(backend->sets.size(), 1);
        QCOMPARE(backend->sets.first(),
                 (QVariantMap{{QStringLiteral("codec"), 2}}));
        QCOMPARE(page.codec->currentData().toInt(), 2);
        QVERIFY2(!page.suggestion->isVisible(), "a chosen codec is no longer a suggestion");
        QVERIFY2(page.codec->accessibleDescription().isEmpty(), "and is not announced as one");
    }

    // CONTROL: stored, so nothing to label; a change to another entry is still
    // exactly one write (activated is emitted once per choice).
    void storedCodecShowsNoLineAndAChangeWritesOnce()
    {
        RadioModel model;
        Hl2HardwareBackend* backend = install(model, 2, false);
        RadioSetupDialog dialog(&model);
        const Page page = openPage(dialog);
        QVERIFY2(page.codec && page.suggestion, "codec combo and suggestion line found");
        QVERIFY2(!page.suggestion->isVisible(), "a stored codec carries no suggestion line");
        QVERIFY2(page.codec->accessibleDescription().isEmpty(), "nor an announced one");
        QVERIFY2(backend->sets.isEmpty(), "opening the page writes nothing");

        page.codec->setFocus();
        QTest::keyClick(page.codec, Qt::Key_Up);
        QCOMPARE(backend->sets.size(), 1);
        QCOMPARE(backend->sets.first(),
                 (QVariantMap{{QStringLiteral("codec"), 1}}));
        QVERIFY2(!page.suggestion->isVisible(), "still no line");
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-codec-suggestion-dialog"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    Hl2CodecSuggestionDialogTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "hl2_codec_suggestion_dialog_test.moc"
