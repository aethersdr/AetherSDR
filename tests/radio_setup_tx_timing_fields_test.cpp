// Radio Setup → Transmit → Timings: ACC TX, TX Delay, RCA TX1-3 and Timeout
// write `interlock set <key>=`, which only a command plane carries (#5370).
//
// Pinned here, each with its control:
//   1. connected with no command plane, every field is dimmed (disabled, not
//      hidden) and says why on the tooltip AND accessibleDescription;
//   2. finishing an edit there sends nothing and raises no commandDropped;
//   3. on disconnect the fields let go, and dim again on the next connect;
//   4. with a command plane every field is enabled, carries no reason, and an
//      edit sends the same `interlock set` text as before, Timeout in ms.
//
// No hardware and no transport: the no-command-plane radio is an injected
// backend that reports itself connected, and the command-plane radios are a
// bare model on the Flex backend and the Demo, whose connection is synthetic
// and never dialled.

#include "TestSettingsProfile.h"
#include "core/RadioDiscovery.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/sim/SimBackend.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QGridLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QLoggingCategory>
#include <QSignalSpy>
#include <QStringList>
#include <QtTest>

#include <memory>

using namespace AetherSDR;

namespace {

class NoCommandPlaneBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{true};
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override { connected = true; }
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

NoCommandPlaneBackend* installNoCommandPlaneRadio(RadioModel& model)
{
    auto owned = std::make_unique<NoCommandPlaneBackend>();
    NoCommandPlaneBackend* backend = owned.get();
    backend->caps.family = QStringLiteral("hl2");
    model.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
    return backend;
}

QStringList* g_logSink = nullptr;

void captureLogHandler(QtMsgType, const QMessageLogContext&, const QString& msg)
{
    if (g_logSink) {
        *g_logSink << msg;
    }
}

// RadioModel has no outbound-command signal; sendCommand() logs each command
// at debug on aether.protocol, quoted, so a quoted fragment is the whole text.
class ScopedCommandLog
{
public:
    ScopedCommandLog()
    {
        g_logSink = &m_lines;
        m_previous = qInstallMessageHandler(captureLogHandler);
        QLoggingCategory::setFilterRules(QStringLiteral("aether.protocol.debug=true"));
    }
    ~ScopedCommandLog()
    {
        QLoggingCategory::setFilterRules(QString());
        qInstallMessageHandler(m_previous);
        g_logSink = nullptr;
    }
    ScopedCommandLog(const ScopedCommandLog&) = delete;
    ScopedCommandLog& operator=(const ScopedCommandLog&) = delete;

    int count(const QString& fragment) const
    {
        int n = 0;
        for (const QString& line : m_lines) {
            if (line.contains(fragment)) {
                ++n;
            }
        }
        return n;
    }

private:
    QStringList      m_lines;
    QtMessageHandler m_previous{nullptr};
};

struct TimingField {
    const char* caption;
    const char* accessibleName;
    const char* typed;
    const char* wireText;   // as sendCommand logs it, quotes included
};

// Every field the Timings group wires to `interlock set`.
const TimingField kFields[] = {
    {"ACC TX:",        "ACC TX",        "11", "\"interlock set acc_tx_delay=11\""},
    {"TX Delay:",      "TX Delay",      "30", "\"interlock set tx_delay=30\""},
    {"RCA TX1:",       "RCA TX1",       "12", "\"interlock set tx1_delay=12\""},
    {"Timeout (sec):", "Timeout (sec)", "45", "\"interlock set timeout=45000\""},
    {"RCA TX2:",       "RCA TX2",       "13", "\"interlock set tx2_delay=13\""},
    {"RCA TX3:",       "RCA TX3",       "14", "\"interlock set tx3_delay=14\""},
};

// Found from the caption the operator reads, so the lookup does not depend on
// anything the gate itself sets.
QLineEdit* findTimingEdit(QWidget& dialog, const QString& caption)
{
    for (QGridLayout* grid : dialog.findChildren<QGridLayout*>()) {
        for (int i = 0; i < grid->count(); ++i) {
            auto* label = qobject_cast<QLabel*>(grid->itemAt(i)->widget());
            if (!label || label->text() != caption) {
                continue;
            }
            int row = 0, col = 0, rowSpan = 0, colSpan = 0;
            grid->getItemPosition(i, &row, &col, &rowSpan, &colSpan);
            QLayoutItem* item = grid->itemAtPosition(row, col + 1);
            return item ? qobject_cast<QLineEdit*>(item->widget()) : nullptr;
        }
    }
    return nullptr;
}

void connectDemo(RadioModel& model)
{
    RadioInfo demo;
    demo.name = QStringLiteral("FLEX-6700");
    demo.model = SimBackend::demoModelName();
    demo.serial = SimBackend::demoSerial();
    demo.family = SimBackend::familyName();
    demo.address = QHostAddress(QHostAddress::LocalHost);  // never dialled
    demo.port = 4992;
    model.connectToRadio(demo);
}

} // namespace

class RadioSetupTxTimingFieldsTest : public QObject {
    Q_OBJECT
private slots:

    void noCommandPlaneDimsEveryFieldAndSendsNothing()
    {
        RadioModel model;
        NoCommandPlaneBackend* backend = installNoCommandPlaneRadio(model);
        // The premises, asserted rather than assumed.
        QVERIFY2(model.isConnected(), "the injected radio reports connected");
        QVERIFY2(!model.hasCommandPlane(), "no command plane");

        QSignalSpy dropped(&model, &RadioModel::commandDropped);

        RadioSetupDialog dialog(&model);
        dialog.show();
        dialog.selectTab(QStringLiteral("Transmit"));

        QList<QLineEdit*> edits;
        for (const TimingField& field : kFields) {
            QLineEdit* edit = findTimingEdit(dialog, QLatin1String(field.caption));
            QVERIFY2(edit, field.caption);
            edits.append(edit);

            QVERIFY2(edit->isVisible(), field.caption);
            QVERIFY2(!edit->isEnabled(), field.caption);
            QVERIFY2(edit->accessibleDescription().startsWith(
                         QStringLiteral("Unavailable:")),
                     qPrintable(edit->accessibleDescription()));
            QCOMPARE(edit->toolTip(), edit->accessibleDescription());
            QCOMPARE(edit->accessibleName(), QLatin1String(field.accessibleName));

            // A value that arrives anyway (automation, a queued edit).
            ScopedCommandLog log;
            edit->setText(QLatin1String(field.typed));
            emit edit->editingFinished();
            QCOMPARE(log.count(QStringLiteral("interlock set")), 0);
        }
        QCOMPARE(dropped.count(), 0);

        // The radio goes away: the gate lets go.
        backend->connected = false;
        emit model.connectionStateChanged(false);
        for (QLineEdit* edit : edits) {
            QVERIFY2(edit->isEnabled(), "disconnected: live again");
            QVERIFY(edit->accessibleDescription().isEmpty());
            QVERIFY(edit->toolTip().isEmpty());
        }

        // And back: dimmed again, with the reason.
        backend->connected = true;
        emit model.connectionStateChanged(true);
        for (QLineEdit* edit : edits) {
            QVERIFY(!edit->isEnabled());
            QVERIFY(edit->accessibleDescription().startsWith(QStringLiteral("Unavailable:")));
        }
        QCOMPARE(dropped.count(), 0);
    }

    // POSITIVE CONTROL: with a command plane each field is enabled and sends
    // its `interlock set` text exactly once, so the assertions above cannot be
    // passed by fields dimmed, or disconnected from their write, on every radio.
    void commandPlaneSendsTheSameWireText()
    {
        RadioModel model;  // a bare model is on the Flex backend
        QVERIFY2(model.hasCommandPlane(), "Flex has a command plane");

        RadioSetupDialog dialog(&model);
        dialog.show();
        dialog.selectTab(QStringLiteral("Transmit"));

        for (const TimingField& field : kFields) {
            QLineEdit* edit = findTimingEdit(dialog, QLatin1String(field.caption));
            QVERIFY2(edit, field.caption);
            QVERIFY2(edit->isEnabled(), field.caption);
            QVERIFY2(edit->accessibleDescription().isEmpty(), field.caption);
            QVERIFY2(edit->toolTip().isEmpty(), field.caption);

            ScopedCommandLog log;
            edit->setText(QLatin1String(field.typed));
            emit edit->editingFinished();
            QVERIFY2(log.count(QLatin1String(field.wireText)) == 1, field.wireText);
            QCOMPARE(log.count(QStringLiteral("interlock set")), 1);
            QCOMPARE(edit->text(), QLatin1String(field.typed));
        }

        // Unchanged clamp: a negative entry is sent, and shown, as 0.
        QLineEdit* txDelay = findTimingEdit(dialog, QStringLiteral("TX Delay:"));
        QVERIFY(txDelay);
        ScopedCommandLog log;
        txDelay->setText(QStringLiteral("-5"));
        emit txDelay->editingFinished();
        QCOMPARE(log.count(QStringLiteral("\"interlock set tx_delay=0\"")), 1);
        QCOMPARE(txDelay->text(), QStringLiteral("0"));
    }

    // A CONNECTED radio with a command plane: the gate keys on the command
    // plane, not on being connected.
    void connectedCommandPlaneStaysLive()
    {
        RadioModel model;
        connectDemo(model);
        QTRY_VERIFY_WITH_TIMEOUT(model.isConnected(), 5000);
        QVERIFY2(model.hasCommandPlane(), "the Demo has a command plane");

        QSignalSpy dropped(&model, &RadioModel::commandDropped);

        RadioSetupDialog dialog(&model);
        dialog.show();
        dialog.selectTab(QStringLiteral("Transmit"));

        for (const TimingField& field : kFields) {
            QLineEdit* edit = findTimingEdit(dialog, QLatin1String(field.caption));
            QVERIFY2(edit, field.caption);
            QVERIFY2(edit->isEnabled(), field.caption);
            QVERIFY2(edit->accessibleDescription().isEmpty(), field.caption);

            ScopedCommandLog log;
            edit->setText(QLatin1String(field.typed));
            emit edit->editingFinished();
            QVERIFY2(log.count(QLatin1String(field.wireText)) == 1, field.wireText);
        }
        QCOMPARE(dropped.count(), 0);
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-tx-timing-fields"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    RadioSetupTxTimingFieldsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "radio_setup_tx_timing_fields_test.moc"
