#include "gui/Kxpa100AdvancedWidget.h"
#include "models/Kxpa100Model.h"
#include "TestSettingsProfile.h"
#include <QApplication>
#include <QAccessible>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTest>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } } while (false)
namespace AetherSDR {
class Kxpa100ModelTest {
public:
    static void ready(Kxpa100Model& model) {
        model.m_now=[] { return qint64(100); }; model.m_connected=model.m_detected=true; model.m_lastReply=100;
        model.m_status.identity="KXPA100"; model.m_status.serial="12345"; model.m_status.firmware="01.39";
        model.m_status.tuning=false; model.m_status.inputWatts=0; model.m_status.forwardWatts=0; model.m_status.tunerPresent=true;
        model.m_received["^TP"]=model.m_received["^PI"]=model.m_received["^PF"]=100;
    }
    static void removeTuner(Kxpa100Model& model) { model.m_status.tunerPresent=false; emit model.statusUpdated(model.m_status); }
};
class Kxpa100AdvancedWidgetTest {
public:
    static int run() {
        Kxpa100Model model; Kxpa100ModelTest::ready(model); Kxpa100AdvancedWidget widget(&model);
        widget.showStatus("Backup readback completed.");
        auto* accessible=QAccessible::queryAccessibleInterface(widget.m_status);
        CHECK(accessible && accessible->text(QAccessible::Name)==widget.m_status->text());
        auto* table=widget.findChild<QTableWidget*>("kxConfiguration"); CHECK(table && table->rowCount()==12 && !table->isEnabled());
        Kxpa100::Configuration config; config.fill(3); emit model.configurationRead(config);
        CHECK(table->isEnabled()); table->item(1,1)->setCheckState(Qt::Unchecked);
        CHECK(widget.m_staged && (*widget.m_staged)[0]==2);
        table->item(1,2)->setCheckState(Qt::Unchecked); CHECK((*widget.m_staged)[0]==2);
        CHECK(table->item(1,2)->checkState()==Qt::Checked); // Never stage an unconnected band.
        CHECK(widget.findChild<QPushButton*>("kxApplyConfiguration")->isEnabled());
        widget.findChild<QPushButton*>("kxDiscardConfiguration")->click(); CHECK(!widget.m_staged);
        table->item(0,1)->setCheckState(Qt::Unchecked); CHECK(widget.m_staged);
        for (int mask : *widget.m_staged) { CHECK(mask==2); }
        CHECK(widget.loadBackup(QStringLiteral(KXPA100_FIXTURE_PATH)));
        auto* restore=widget.findChild<QPushButton*>("kxRestoreBackup"); CHECK(!restore->isEnabled());
        widget.findChild<QCheckBox*>("kxConfirmRestore")->setChecked(true); CHECK(restore->isEnabled());
        auto* erase=widget.findChild<QPushButton*>("kxEraseMemories"); CHECK(!erase->isEnabled());
        widget.findChild<QCheckBox*>("kxConfirmErase")->setChecked(true); CHECK(erase->isEnabled());
        widget.findChild<QComboBox*>("kxEraseAntenna")->setCurrentIndex(1); CHECK(!erase->isEnabled());
        CHECK(widget.findChild<QComboBox*>("kxEraseAntenna")->count()==3);
        Kxpa100ModelTest::removeTuner(model);
        CHECK(!table->isEnabled() && !widget.findChild<QPushButton*>("kxApplyConfiguration")->isEnabled());
        CHECK(widget.findChild<QPushButton*>("kxSaveBackup")->isEnabled());
        model.disconnectDevice(); CHECK(!restore->isEnabled() && !table->isEnabled());
        auto* transient=new Kxpa100AdvancedWidget(nullptr); transient->show();
        transient->findChild<QPushButton*>("kxChooseBackup")->click(); QTest::qWait(10);
        QPointer<QFileDialog> dialog=transient->findChild<QFileDialog*>("kxMaintenanceFileDialog"); CHECK(dialog);
        delete transient; CHECK(dialog && !dialog->parent() && !dialog->isVisible());
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); CHECK(dialog.isNull());
        return 0;
    }
};
}
int main(int argc,char** argv) { TestSettingsProfile profile("kxpa100-advanced"); QApplication app(argc,argv); return AetherSDR::Kxpa100AdvancedWidgetTest::run(); }
