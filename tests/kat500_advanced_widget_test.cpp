#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/Kat500AdvancedWidget.h"
#include "models/Kat500Model.h"
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QFileDialog>
#include <QPointer>
#include <QPushButton>
#include <QHeaderView>
#include <QScrollBar>
#include <QTableWidget>
#include <QTemporaryFile>
#include <QTest>
#include <cstdio>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                             \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (false)
namespace AetherSDR
{
class Kat500ModelTest {
public:
    static void futureFirmware(Kat500Model& model) {
        model.m_connected = true; model.m_detected = true; model.m_now=[] {return qint64(100);};
        model.m_lastReply=100; model.m_status.identity="KAT500"; model.m_status.firmware="02.17";
        model.m_status.powerOn=true; model.m_status.tuning=false; model.m_status.forwardAdc=0;
        model.m_received["TP"]=100; model.m_received["VFWD"]=100;
        emit model.statusUpdated(model.m_status);
    }
};
class Kat500AdvancedWidgetTest
{
  public:
    static int run()
    {
        Kat500Model model;
        Kat500AdvancedWidget widget(&model);
        widget.resize(335, 1200);
        widget.show();
        QTest::qWait(30);
        CHECK(!widget.findChild<QPushButton*>("katOfficialUtility"));
        CHECK(!widget.findChild<QPushButton*>("katOfficialHelp"));
        CHECK(widget.m_configuration->font().pixelSize() == 10);
        CHECK(widget.m_configuration->horizontalScrollBar()->maximum() == 0);
        CHECK(widget.findChild<QTableWidget*>("katConfiguration"));
        auto* erase = widget.findChild<QPushButton*>("katEraseMemories");
        CHECK(erase->text() == QStringLiteral("Clear all ATU memories…"));
        CHECK(!erase->isEnabled());
        widget.m_eraseConfirm->setChecked(true);
        widget.m_eraseBand->setCurrentIndex(1);
        CHECK(!widget.m_eraseConfirm->isChecked());
        CHECK(erase->text() == QStringLiteral("Erase selected memories"));
        widget.m_eraseBand->setCurrentIndex(0);

        CHECK(!widget.findChild<QPushButton*>("katApplyConfiguration")->isEnabled());
        Kat500::Configuration config;
        for (const auto key : Kat500::configurationKeys()) {
            config[key] = 1;
        }
        config[{Kat500::Setting::SlotsPerBin, 0, 0}] = 2;
        widget.configurationRead(config);
        auto* bands = widget.findChild<QComboBox*>("katConfigurationBand");
        CHECK(bands && bands->count() == 12);
        bands->setCurrentIndex(0);
        CHECK(widget.m_configuration->item(6, 1)->text() == QStringLiteral("Mixed"));
        CHECK(widget.m_staged.isEmpty());
        for (int row = 0; row < widget.m_configuration->rowCount(); ++row) {
            const auto* item = widget.m_configuration->item(row, 0);
            CHECK(widget.m_configuration->fontMetrics().horizontalAdvance(item->text()) + 8 <=
                  widget.m_configuration->columnWidth(0));
        }
        widget.m_configuration->item(6, 1)->setText("3");
        CHECK(widget.m_staged.size() == 11);
        for (const auto key : widget.m_staged.keys()) {
            CHECK(key.setting == Kat500::Setting::SlotsPerBin);
        }
        widget.findChild<QPushButton*>("katDiscardConfiguration")->click();
        CHECK(widget.m_staged.isEmpty());
        Kat500ModelTest::futureFirmware(model);
        CHECK(!widget.findChild<QPushButton*>("katReadConfiguration")->isEnabled());
        CHECK(widget.findChild<QPushButton*>("katReadConfiguration")->accessibleDescription().contains("02.16"));
        QTemporaryFile oversized;
        CHECK(oversized.open());
        oversized.write(QByteArray(262145, 'x'));
        oversized.flush();
        CHECK(!widget.loadBackup(oversized.fileName()));
        auto* transient = new Kat500AdvancedWidget(&model);
        transient->show();
        transient->findChild<QPushButton*>("katChooseBackup")->click();
        QTest::qWait(250);
        QPointer<QFileDialog> dialog = transient->findChild<QFileDialog*>("katMaintenanceFileDialog");
        CHECK(dialog);
        delete transient;
        CHECK(dialog && !dialog->isVisible() && !dialog->parent());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTest::qWait(250);
        CHECK(dialog.isNull());
        return 0;
    }
};
} // namespace AetherSDR
int main(int argc, char** argv)
{
    TestSettingsProfile profile("kat500-advanced-widget-test");
    QApplication app(argc, argv);
    AetherSDR::AppSettings::instance().load();
    return AetherSDR::Kat500AdvancedWidgetTest::run();
}
