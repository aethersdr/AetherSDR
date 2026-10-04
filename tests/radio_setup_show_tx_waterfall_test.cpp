// Radio Setup → Transmit's "Show TX in Waterfall" button on the production
// dialog. Where the waterfall rows are made on this host the click sets the
// transmit model and no wire text is sent; on a Flex the click sends the wire
// text and the model waits for the radio's echo. No transport: an injected
// backend, and a bare model on the never-connected Flex backend.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/IRadioBackend.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QGridLayout>
#include <QLabel>
#include <QLoggingCategory>
#include <QPushButton>
#include <QStringList>
#include <QtTest>

#include <memory>

using namespace AetherSDR;

namespace {

QStringList* g_logSink = nullptr;

void captureLogHandler(QtMsgType, const QMessageLogContext&, const QString& msg)
{
    if (g_logSink) {
        *g_logSink << msg;
    }
}

// RadioModel has no outbound-command signal; sendCommand() logs each command
// at debug on aether.protocol.
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

    bool contains(const QString& fragment) const
    {
        for (const QString& line : m_lines) {
            if (line.contains(fragment)) {
                return true;
            }
        }
        return false;
    }

private:
    QStringList      m_lines;
    QtMessageHandler m_previous{nullptr};
};

class NoCommandPlaneBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
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
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

// Found from the caption the operator reads, so the lookup does not depend on
// an object name.
QPushButton* findShowTxButton(QWidget& dialog)
{
    for (QGridLayout* grid : dialog.findChildren<QGridLayout*>()) {
        for (int i = 0; i < grid->count(); ++i) {
            auto* caption = qobject_cast<QLabel*>(grid->itemAt(i)->widget());
            if (!caption || caption->text() != QStringLiteral("Show TX in Waterfall:")) {
                continue;
            }
            int row = 0, col = 0, rowSpan = 0, colSpan = 0;
            grid->getItemPosition(i, &row, &col, &rowSpan, &colSpan);
            QLayoutItem* item = grid->itemAtPosition(row, col + 1);
            return item ? qobject_cast<QPushButton*>(item->widget()) : nullptr;
        }
    }
    return nullptr;
}

const QString kWireText = QStringLiteral("transmit set show_tx_in_waterfall=");

} // namespace

class RadioSetupShowTxWaterfallTest : public QObject {
    Q_OBJECT
private slots:

    void hostMadeRowsTakeTheFlagInTheModel()
    {
        RadioModel model;
        auto backend = std::make_unique<NoCommandPlaneBackend>();
        backend->caps.family = QStringLiteral("hl2");
        model.setBackendForTest(std::move(backend), QStringLiteral("hl2"));
        QVERIFY2(model.shapesDisplayRatesLocally(), "the rows are made on this host");
        QVERIFY(!model.transmitModel().showTxInWaterfall());

        RadioSetupDialog dialog(&model);
        dialog.show();
        dialog.selectTab(QStringLiteral("Transmit"));
        QPushButton* button = findShowTxButton(dialog);
        QVERIFY2(button, "Show TX in Waterfall button found");
        QVERIFY(!button->isChecked());

        ScopedCommandLog log;
        button->click();
        QVERIFY2(model.transmitModel().showTxInWaterfall(), "the click sets the model");
        QCOMPARE(button->text(), QStringLiteral("Enabled"));
        QVERIFY2(!log.contains(kWireText), "no wire text for a radio that cannot take it");

        button->click();
        QVERIFY2(!model.transmitModel().showTxInWaterfall(), "and clears it");
        QCOMPARE(button->text(), QStringLiteral("Disabled"));
        QVERIFY(!log.contains(kWireText));
    }

    // POSITIVE CONTROL: on a Flex the click sends the wire text and the model
    // is not set optimistically, so the slot above cannot pass on a button
    // that takes the flag locally on every radio.
    void flexStillSendsTheWireText()
    {
        RadioModel model;  // a bare model is on the Flex backend
        QVERIFY(!model.shapesDisplayRatesLocally());

        RadioSetupDialog dialog(&model);
        dialog.show();
        dialog.selectTab(QStringLiteral("Transmit"));
        QPushButton* button = findShowTxButton(dialog);
        QVERIFY2(button, "Show TX in Waterfall button found");

        ScopedCommandLog log;
        button->click();
        QVERIFY2(log.contains(kWireText + QLatin1Char('1')), "the radio is told");
        QVERIFY2(!model.transmitModel().showTxInWaterfall(), "the model waits for the echo");
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-show-tx-waterfall"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    RadioSetupShowTxWaterfallTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "radio_setup_show_tx_waterfall_test.moc"
