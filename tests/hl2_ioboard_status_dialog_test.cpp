// Radio Setup -> HL2 Hardware -> IO Board: the `hl2` / `ioboard.status` reply
// is shown read-only, on demand.
//
// Pinned here:
//   1. opening the page sends no read and writes nothing;
//   2. the button (mouse or keyboard) sends exactly one ioboard.status, and the
//      reply's registers are shown in the status line;
//   3. "No IO board answered." reads as no board, any other error as "Not read";
//   4. the button is dimmed while a read is pending and while disconnected;
//   5. a screen reader gets the reading itself, not only the line's label;
//   6. a connection change forgets the reading and drops a pending read's answer.
//
// No socket and no radio: an injected backend that declares the hl2
// extension and answers ioboard.status with the shape Hl2Backend replies with.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/hl2/MetisProtocol.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QAccessible>
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

    // Hl2Backend's reply map for the registers word the board sent.
    static QVariantMap replyFor(std::uint32_t registers)
    {
        const hl2::IoBoardStatus s = hl2::decodeIoBoardStatus(registers);
        return {{QStringLiteral("inputPins"), s.inputPins},
                {QStringLiteral("receiving"), s.receiving()},
                {QStringLiteral("in1"), s.input(1)},
                {QStringLiteral("in2"), s.input(2)},
                {QStringLiteral("in3"), s.input(3)},
                {QStringLiteral("in4"), s.input(4)},
                {QStringLiteral("in5"), s.input(5)},
                {QStringLiteral("antennaTuner"), s.antennaTuner},
                {QStringLiteral("fault"), s.fault},
                {QStringLiteral("firmwareMajor"), s.firmwareMajor}};
    }
    // Firmware 1, receiving, In2 high, a tune in progress (0xEE), no fault.
    std::uint32_t registers{0x0100EE05};
    void finish() { emit extensionResult(std::exchange(pendingId, 0), replyFor(registers)); }

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
            emit extensionResult(id, replyFor(registers));
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

// What a screen reader announces for the status line.
QString spokenName(QLabel* label)
{
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(label);
    return iface ? iface->text(QAccessible::Name) : QString();
}

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
        QCOMPARE(spokenName(page.status), QStringLiteral("IO board status: Not read."));
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
        QVERIFY2(spokenName(page.status).contains(QStringLiteral("Firmware major: 1")),
                 qPrintable(spokenName(page.status)));
        QVERIFY2(page.read->isEnabled(), "a finished read can be repeated");
        QCOMPARE(backend->writes, 0);
    }

    // Firmware 3, transmitting, In1 and In5 high, tune failed (0xF2), fault 3.
    void failedTuneAndFaultAreShownInHex()
    {
        RadioModel model;
        IoBoardBackend* backend = install(model, IoBoardBackend::Answer::Board);
        backend->registers = 0x0303F222;
        RadioSetupDialog dialog(&model);
        const Page page = openPage(dialog);
        QVERIFY2(page.read && page.status, "IO board button and status line found");
        page.read->click();
        QCOMPARE(page.status->text(),
                 QStringLiteral("Firmware major: 3\nEXTTR: low (transmit)\n"
                                "Inputs: In1 high, In2 low, In3 low, In4 low, In5 high\n"
                                "Antenna tuner: tune failed (0xf2)\nFault: code 0x03"));
    }

    // An ATU code the board defines no meaning for is shown raw.
    void undefinedTunerCodeIsShownRaw()
    {
        RadioModel model;
        IoBoardBackend* backend = install(model, IoBoardBackend::Answer::Board);
        backend->registers = 0x01004201;
        RadioSetupDialog dialog(&model);
        const Page page = openPage(dialog);
        QVERIFY2(page.read && page.status, "IO board button and status line found");
        page.read->click();
        QVERIFY2(page.status->text().contains(QStringLiteral("\nAntenna tuner: 0x42\nFault: none")),
                 qPrintable(page.status->text()));
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
        QVERIFY2(spokenName(page.status).contains(QStringLiteral("No IO board answered")),
                 qPrintable(spokenName(page.status)));
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

    void connectionChangeForgetsTheReading()
    {
        RadioModel model;
        install(model, IoBoardBackend::Answer::Board);
        RadioSetupDialog dialog(&model);
        const Page page = openPage(dialog);
        QVERIFY2(page.read && page.status, "IO board button and status line found");
        page.read->click();
        QVERIFY(page.status->text().startsWith(QStringLiteral("Firmware major: 1")));
        emit model.connectionStateChanged(true);
        QCOMPARE(page.status->text(), QStringLiteral("Not read."));
        QCOMPARE(spokenName(page.status), QStringLiteral("IO board status: Not read."));
        QVERIFY(page.read->isEnabled());
    }

    // The previous connection's answer must not land after "Not read.".
    void lateAnswerAfterAConnectionChangeIsDropped()
    {
        RadioModel model;
        IoBoardBackend* backend = install(model, IoBoardBackend::Answer::Later);
        RadioSetupDialog dialog(&model);
        const Page page = openPage(dialog);
        QVERIFY2(page.read && page.status, "IO board button and status line found");
        page.read->click();
        QCOMPARE(page.status->text(), QStringLiteral("Reading…"));
        emit model.connectionStateChanged(true);
        QVERIFY2(page.read->isEnabled(), "a new connection can be read");
        backend->finish();
        QCOMPARE(page.status->text(), QStringLiteral("Not read."));
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
