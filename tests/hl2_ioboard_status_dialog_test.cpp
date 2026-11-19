// Radio Setup -> HL2 Hardware -> IO Board: the `hl2` / `ioboard.status` reply
// is shown read-only, on demand.
//
// Pinned here:
//   1. opening the page sends no read and writes nothing;
//   2. the button (mouse or keyboard) sends exactly one ioboard.status, and the
//      reply's registers are shown in the status line;
//   3. "No IO board answered." reads as no board, any other error as "Not read";
//   4. the button is dimmed while a read is pending and while disconnected.
//
// No socket and no radio: an injected backend that declares the hl2
// extension and answers ioboard.status with the shape Hl2Backend replies with.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/IRadioBackend.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QtTest>

#include <memory>
#include <utility>

namespace AetherSDR {
// RadioModel befriends this name; the identity is what makes the page live.
struct RadioModelWakeTestAccess {
    static void identity(RadioModel& radio, const QString& serial)
    { radio.m_lastInfo.serial = serial; }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {

class IoBoardBackend final : public IRadioBackend {
public:
    enum class Answer { Board, NoBoard, Busy, Later };
    RadioCapabilities caps;
    bool connected{true};
    Answer answer{Answer::Board};
    int reads{0};
    int writes{0};
    quint64 pendingId{0};
    IoBoardBackend()
    {
        caps.family = QStringLiteral("hl2");
        caps.extensionNamespaces = {QStringLiteral("hl2")};
    }
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}

    // Hl2Backend's reply for a board at firmware 1, receiving, In2 high,
    // a tune in progress and no fault (decodeIoBoardStatus(0x0100EE05)).
    static QVariantMap boardReply()
    {
        return {{QStringLiteral("inputPins"), 0x05},
                {QStringLiteral("receiving"), true},
                {QStringLiteral("in1"), false},
                {QStringLiteral("in2"), true},
                {QStringLiteral("in3"), false},
                {QStringLiteral("in4"), false},
                {QStringLiteral("in5"), false},
                {QStringLiteral("antennaTuner"), 0xEE},
                {QStringLiteral("fault"), 0},
                {QStringLiteral("firmwareMajor"), 1}};
    }
    void finish() { emit extensionResult(std::exchange(pendingId, 0), boardReply()); }

    void invokeExtension(const QString& ns, const QString& verb, quint64 id,
                         const QVariant&) override
    {
        if (ns != QLatin1String("hl2"))
            return;
        if (verb == QLatin1String("hw.set")) {
            ++writes;
            return;
        }
        if (verb != QLatin1String("ioboard.status") || id == 0)
            return;   // hw.get and the Calibration page's verbs get no reply
        ++reads;
        switch (answer) {
        case Answer::Board:
            emit extensionResult(id, boardReply());
            break;
        case Answer::NoBoard:
            emit extensionError(id, QStringLiteral("No IO board answered."));
            break;
        case Answer::Busy:
            emit extensionError(id, QStringLiteral("the radio's I2C bus was busy"));
            break;
        case Answer::Later:
            pendingId = id;
            break;
        }
    }
};

struct Page {
    QPushButton* read{nullptr};
    QLabel* status{nullptr};
};

Page openPage(RadioSetupDialog& dialog)
{
    dialog.show();
    dialog.selectTab(QStringLiteral("HL2 Hardware"));
    return {dialog.findChild<QPushButton*>(QStringLiteral("hl2HwIoBoardRead")),
            dialog.findChild<QLabel*>(QStringLiteral("hl2HwIoBoardStatus"))};
}

IoBoardBackend* install(RadioModel& model, IoBoardBackend::Answer answer,
                        bool connected = true)
{
    auto owned = std::make_unique<IoBoardBackend>();
    IoBoardBackend* backend = owned.get();
    backend->answer = answer;
    backend->connected = connected;
    model.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
    RadioModelWakeTestAccess::identity(model, QStringLiteral("00:1C:C0:A2:13:DD"));
    return backend;
}

} // namespace

class Hl2IoBoardStatusDialogTest : public QObject {
    Q_OBJECT
private slots:

    void openingThePageReadsNothing()
    {
        RadioModel model;
        IoBoardBackend* backend = install(model, IoBoardBackend::Answer::Board);
        RadioSetupDialog dialog(&model);
        const Page page = openPage(dialog);
        QVERIFY2(page.read && page.status, "IO board button and status line found");
        QCOMPARE(page.status->text(), QStringLiteral("Not read."));
        QVERIFY2(page.read->isEnabled(), "a connected HL2 can be read");
        QVERIFY2(!page.read->accessibleName().isEmpty(), "the button has an accessible name");
        QVERIFY2(!page.status->accessibleName().isEmpty(), "the line has an accessible name");
        QCOMPARE(backend->reads, 0);
        QCOMPARE(backend->writes, 0);
    }

    void readShowsTheBoardsRegisters()
    {
        RadioModel model;
        IoBoardBackend* backend = install(model, IoBoardBackend::Answer::Board);
        RadioSetupDialog dialog(&model);
        const Page page = openPage(dialog);
        QVERIFY2(page.read && page.status, "IO board button and status line found");
        page.read->click();
        QCOMPARE(backend->reads, 1);
        QCOMPARE(page.status->text(),
                 QStringLiteral("Firmware major: 1\nEXTTR: high (receive)\n"
                                "Inputs: In1 low, In2 high, In3 low, In4 low, In5 low\n"
                                "Antenna tuner: tuning, asks for RF (0xee)\nFault: none"));
        QVERIFY2(page.read->isEnabled(), "a finished read can be repeated");
        QCOMPARE(backend->writes, 0);
    }

    void keyboardActivationReads()
    {
        RadioModel model;
        IoBoardBackend* backend = install(model, IoBoardBackend::Answer::Board);
        RadioSetupDialog dialog(&model);
        const Page page = openPage(dialog);
        QVERIFY2(page.read, "IO board button found");
        page.read->setFocus();
        QTest::keyClick(page.read, Qt::Key_Space);
        QCOMPARE(backend->reads, 1);
        QVERIFY(page.status->text().startsWith(QStringLiteral("Firmware major: 1")));
    }

    void noBoardSaysSo()
    {
        RadioModel model;
        install(model, IoBoardBackend::Answer::NoBoard);
        RadioSetupDialog dialog(&model);
        const Page page = openPage(dialog);
        QVERIFY2(page.read && page.status, "IO board button and status line found");
        page.read->click();
        QVERIFY2(page.status->text().startsWith(QStringLiteral("No IO board answered")),
                 qPrintable(page.status->text()));
    }

    void otherErrorsAreNotAReading()
    {
        RadioModel model;
        install(model, IoBoardBackend::Answer::Busy);
        RadioSetupDialog dialog(&model);
        const Page page = openPage(dialog);
        QVERIFY2(page.read && page.status, "IO board button and status line found");
        page.read->click();
        QCOMPARE(page.status->text(), QStringLiteral("Not read: the radio's I2C bus was busy"));
        QVERIFY2(page.read->isEnabled(), "a failed read can be retried");
    }

    void buttonIsDimmedWhileAReadIsPending()
    {
        RadioModel model;
        IoBoardBackend* backend = install(model, IoBoardBackend::Answer::Later);
        RadioSetupDialog dialog(&model);
        const Page page = openPage(dialog);
        QVERIFY2(page.read && page.status, "IO board button and status line found");
        page.read->click();
        QCOMPARE(page.status->text(), QStringLiteral("Reading…"));
        QVERIFY2(!page.read->isEnabled(), "one read at a time");
        page.read->click();
        QCOMPARE(backend->reads, 1);
        backend->finish();
        QVERIFY(page.status->text().startsWith(QStringLiteral("Firmware major: 1")));
        QVERIFY(page.read->isEnabled());
    }

    // CONTROL: nothing to read from a radio that is not connected.
    void disconnectedRadioCannotBeRead()
    {
        RadioModel model;
        IoBoardBackend* backend =
            install(model, IoBoardBackend::Answer::Board, /*connected=*/false);
        RadioSetupDialog dialog(&model);
        const Page page = openPage(dialog);
        QVERIFY2(page.read, "IO board button found");
        QVERIFY2(!page.read->isEnabled(), "dimmed while disconnected");
        QCOMPARE(backend->reads, 0);
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-ioboard-status-dialog"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    Hl2IoBoardStatusDialogTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "hl2_ioboard_status_dialog_test.moc"
