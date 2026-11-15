// Radio Setup spreads side-by-side caption: value pairs across the window's
// width, and keeps each caption beside its value.
//
// The GPS page is the case it was found on: its five rows of two pairs were
// packed against the left edge, leaving the right half of the page empty.
// Transmit → Timings is the same grid with a value spanning the caption's
// place (the TX profile and TX Band Settings), and it must spread too.
//
// No hardware and no transport: a bare RadioModel, never connected, so the
// GPS page is listed (it is hidden only for a connected radio without GPS).
// Set AETHER_PAIR_GRID_GRAB to a directory to save a grab of each page.

#include "TestSettingsProfile.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"
#include "models/Kat500Model.h"
#include "core/PeripheralSettings.h"
#include <QListWidget>
#include <QPushButton>
#include <QFileDialog>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>

#include <QApplication>
#include <QLabel>
#include <QGridLayout>
#include <QtTest>

using namespace AetherSDR;

namespace {

QLabel* caption(QWidget* root, const char* text)
{
    for (QLabel* l : root->findChildren<QLabel*>()) {
        if (l->text() == QLatin1String(text) && l->isVisible())
            return l;
    }
    return nullptr;
}

int leftIn(QWidget* w, QWidget* root) { return w->mapTo(root, QPoint(0, 0)).x(); }
int rightIn(QWidget* w, QWidget* root) { return leftIn(w, root) + w->width(); }

// The grid, at any depth under `layout`, that holds `w`.
QGridLayout* gridHolding(QLayout* layout, QWidget* w)
{
    if (!layout)
        return nullptr;
    auto* grid = qobject_cast<QGridLayout*>(layout);
    if (grid && grid->indexOf(w) >= 0)
        return grid;
    for (int i = 0; i < layout->count(); ++i) {
        if (QGridLayout* found = gridHolding(layout->itemAt(i)->layout(), w))
            return found;
    }
    return nullptr;
}

// The widget in the same grid row, just right of `label`: its value.
QWidget* valueOf(QLabel* label)
{
    QGridLayout* grid = gridHolding(label->parentWidget()->layout(), label);
    if (!grid)
        return nullptr;
    const int index = grid->indexOf(label);
    int row = 0, column = 0, rowSpan = 0, columnSpan = 0;
    grid->getItemPosition(index, &row, &column, &rowSpan, &columnSpan);
    QLayoutItem* item = grid->itemAtPosition(row, column + 1);
    return item ? item->widget() : nullptr;
}

void grab(QWidget* dialog, const char* name)
{
    const QByteArray dir = qgetenv("AETHER_PAIR_GRID_GRAB");
    if (!dir.isEmpty())
        dialog->grab().save(QString::fromLocal8Bit(dir) + QLatin1Char('/') + QLatin1String(name));
}

} // namespace

class RadioSetupPairGridTest : public QObject {
    Q_OBJECT
private slots:

    void kat500AdvancedShortcutSelectsAndReveals()
    {
        AppSettings::instance().load();
        PeripheralSettings::setVisibleDeviceIds({QStringLiteral("kat500")});
        RadioModel model;
        Kat500Model tuner;
        RadioSetupDialog dialog(&model, nullptr, nullptr, nullptr, nullptr, nullptr,
                                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &tuner);
        dialog.show();
        dialog.revealKat500AdvancedSettings();
        auto* devices = dialog.findChild<QListWidget*>("peripheralDeviceList");
        QVERIFY(devices && devices->currentItem());
        QCOMPARE(devices->currentItem()->data(Qt::UserRole).toString(), QStringLiteral("kat500"));
        auto* toggle = dialog.findChild<QPushButton*>("peripheralAdvanced_kat500");
        QVERIFY(toggle && toggle->isChecked() && toggle->isVisible());
        auto* advanced = dialog.findChild<QWidget*>("kat500Advanced");
        QVERIFY(advanced->isVisible());
        QTest::qWait(30);
        QCOMPARE(dialog.width(), 960);
        bool verticalScroll = false;
        for (QWidget* ancestor = advanced->parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
            if (auto* scroll = qobject_cast<QScrollArea*>(ancestor)) {
                QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
                verticalScroll |= scroll->verticalScrollBar()->maximum() > 0;
                QVERIFY(advanced->width() <= scroll->viewport()->width());
            }
        }
        QVERIFY(verticalScroll);
        grab(&dialog, "kat500-advanced.png");
    }

    void kat500ChooserClosesWithRealParent()
    {
        AppSettings::instance().load();
        PeripheralSettings::setVisibleDeviceIds({QStringLiteral("kat500")});
        RadioModel model;
        Kat500Model tuner;
        QPointer<RadioSetupDialog> dialog = new RadioSetupDialog(
            &model, nullptr, nullptr, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &tuner);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
        dialog->revealKat500AdvancedSettings();
        dialog->findChild<QPushButton*>("katChooseBackup")->click();
        QTest::qWait(250);
        QPointer<QFileDialog> chooser = dialog->findChild<QFileDialog*>("katMaintenanceFileDialog");
        QVERIFY(chooser);
        QVERIFY(dialog->close());
        QTest::qWait(500);
        QVERIFY(dialog.isNull());
        QVERIFY(chooser.isNull());
    }

    void gpsPairsSpreadAndStayTogether()
    {
        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.resize(960, 760);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        dialog.selectTab(QStringLiteral("GPS"));
        QCoreApplication::processEvents();

        QLabel* lat = caption(&dialog, "Latitude:");
        QLabel* lon = caption(&dialog, "Longitude:");
        QLabel* time = caption(&dialog, "UTC Time:");
        QVERIFY(lat && lon && time);
        QWidget* page = lat->parentWidget();
        grab(&dialog, "gps.png");

        // The second pair starts in the page's right half...
        const int pageLeft = leftIn(lat, page);
        const int pageWidth = page->width() - pageLeft;
        QVERIFY2(leftIn(lon, page) - pageLeft >= pageWidth * 2 / 5,
                 qPrintable(QStringLiteral("Longitude at %1 of %2")
                                .arg(leftIn(lon, page) - pageLeft).arg(pageWidth)));
        // ...and both columns of pairs line up.
        QCOMPARE(leftIn(time, page), leftIn(lon, page));

        // Each caption is beside its value, not across a gap.
        for (QLabel* label : {lat, lon}) {
            QWidget* value = valueOf(label);
            QVERIFY2(value, qPrintable(label->text()));
            const int gap = leftIn(value, page) - rightIn(label, page);
            QVERIFY2(gap >= 0 && gap <= 24, qPrintable(QStringLiteral("%1 gap %2")
                                                           .arg(label->text()).arg(gap)));
        }
    }

    void transmitTimingsSpread()
    {
        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.resize(960, 760);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        dialog.selectTab(QStringLiteral("Transmit"));
        QCoreApplication::processEvents();

        QLabel* acc = caption(&dialog, "ACC TX:");
        QLabel* delay = caption(&dialog, "TX Delay:");
        QVERIFY(acc && delay);
        QWidget* group = acc->parentWidget();
        grab(&dialog, "transmit.png");
        QVERIFY2(leftIn(delay, group) >= group->width() * 2 / 5,
                 qPrintable(QStringLiteral("TX Delay at %1 of %2")
                                .arg(leftIn(delay, group)).arg(group->width())));
        QWidget* value = valueOf(delay);
        QVERIFY(value);
        QVERIFY(leftIn(value, group) - rightIn(delay, group) <= 24);
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-pair-grid"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    RadioSetupPairGridTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "radio_setup_pair_grid_test.moc"
