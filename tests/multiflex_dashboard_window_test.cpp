// The multiFLEX Dashboard is a canon dialog.
//
// Pinned here, each with its control:
//   1. it is a non-modal CanonWindow dialog: no native frame, titled multiFLEX
//      Dashboard, under the canon/multiFlex theme scope, and with no Close
//      button of its own (the corner close button and Escape close it);
//   2. the size and place the operator gives it persist under the key it used
//      as a PersistentDialog (MultiFlexDialogGeometry), so geometry saved
//      before the migration (PersistentDialog's base64 saveGeometry()) carries
//      over, and a second window restores the size it saves;
//   3. the Enabled / Disabled button is drawn in color.toggle.success when
//      multiFLEX is on and color.accent.danger when it is off, under Default
//      Dark and Default Light;
//   4. in the station table, the PTT tick is drawn in color.accent.success
//      (a glyph the UI font carries, so no color-emoji fallback grays it), our own
//      station is color.canon.cyan with no Disconnect button, and another
//      station's Disconnect button is drawn in color.accent.danger and asks
//      once, then disables;
//   5. Return or Enter on a station row does not click the Enabled /
//      Disabled toggle, the dialog's default button
//      (setReturnClicksDefault(false));
//   6. a selected row keeps its tick and station colors, a theme change
//      recolors the rows without rebuilding them, hovering the toggle
//      brightens its border to color.canon.aqua, and losing the radio closes
//      the window.
//
// No radio: the model is bare and never connected, so the button's toggle
// changes only the model's local state.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/CanonWindow.h"
#include "gui/MultiFlexDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QDialog>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QtTest>

#include <cstdlib>

using namespace AetherSDR;

namespace {

const char* const kGeometryKey = "MultiFlexDialogGeometry";

QPushButton* findEnableButton(QWidget& window)
{
    return window.findChild<QPushButton*>(QStringLiteral("multiFlexEnableButton"));
}

// Pixels within `slack` of `color` on every channel.
int pixelsNear(const QImage& image, const QColor& color, int slack)
{
    int n = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb px = image.pixel(x, y);
            if (std::abs(qRed(px) - color.red()) <= slack
                && std::abs(qGreen(px) - color.green()) <= slack
                && std::abs(qBlue(px) - color.blue()) <= slack)
                ++n;
        }
    }
    return n;
}

// Feeds two stations into a bare model: client 0x00000000 is us (a bare
// model's own handle is 0), and the other station holds PTT.
void addTwoStations(RadioModel& model)
{
    model.handleStatusForTest(QStringLiteral("client 0x00000000"),
        {{QStringLiteral("connected"), QString()},
         {QStringLiteral("program"), QStringLiteral("AetherSDR")},
         {QStringLiteral("station"), QStringLiteral("Bench")}});
    model.handleStatusForTest(QStringLiteral("client 0x7594C952"),
        {{QStringLiteral("connected"), QString()},
         {QStringLiteral("program"), QStringLiteral("SmartSDR")},
         {QStringLiteral("station"), QStringLiteral("Shack")},
         {QStringLiteral("local_ptt"), QStringLiteral("1")}});
    QCoreApplication::processEvents();
}

int rowOfHandle(QTableWidget* table, quint32 handle)
{
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0)->data(Qt::UserRole).toUInt() == handle)
            return row;
    }
    return -1;
}

int cellPixelsNear(QTableWidget* table, int row, int column, const QColor& color, int slack)
{
    const QRect cell = table->visualItemRect(table->item(row, column));
    return pixelsNear(table->viewport()->grab(cell).toImage(), color, slack);
}

// Returns the process to the theme it started on, on every exit from a slot.
class ThemeRestore {
public:
    ThemeRestore() : m_theme(ThemeManager::instance().activeTheme()) {}
    ~ThemeRestore() { ThemeManager::instance().setActiveTheme(m_theme); }
    Q_DISABLE_COPY_MOVE(ThemeRestore)
private:
    const QString m_theme;
};

} // namespace

class MultiFlexDashboardWindowTest : public QObject {
    Q_OBJECT
private slots:

    void isACanonDialog()
    {
        RadioModel model;
        MultiFlexDialog window(&model);
        QVERIFY(qobject_cast<CanonWindow*>(&window));
        QCOMPARE(window.windowTitle(), QStringLiteral("multiFLEX Dashboard"));
        QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
        // A dialog over its parent, not a workspace, and it leaves the main
        // window usable while it is open.
        QCOMPARE(window.windowType(), Qt::Dialog);
        QCOMPARE(window.windowModality(), Qt::NonModal);
        QCOMPARE(window.property("themeContainer").toString(),
                 QStringLiteral("canon/multiFlex"));
        // CONTROL: the Enabled button is found, so the search below can find
        // a button by its text.
        QVERIFY(findEnableButton(window));
        for (QPushButton* button : window.findChildren<QPushButton*>()) {
            QVERIFY2(button->text() != QLatin1String("Close"),
                     "the corner close button replaces the Close button");
        }
    }

    void geometryPersistsUnderTheOldKeyAndRestores()
    {
        auto& settings = AppSettings::instance();
        settings.remove(QLatin1String(kGeometryKey));
        // CONTROL: nothing saved yet, so the restore below can only come from
        // this slot's own close.
        QVERIFY(settings.value(QLatin1String(kGeometryKey)).toString().isEmpty());

        RadioModel model;
        const QSize chosen(900, 420);
        {
            MultiFlexDialog window(&model);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            window.resize(chosen);
            QCoreApplication::processEvents();
            QCOMPARE(window.size(), chosen);
            // Escape closes it, and the geometry is flushed on hide.
            QTest::keyClick(&window, Qt::Key_Escape);
            QTRY_VERIFY(!window.isVisible());
        }
        const QString saved = settings.value(QLatin1String(kGeometryKey)).toString();
        QVERIFY2(saved.endsWith(QLatin1String(",900,420")), qPrintable(saved));

        MultiFlexDialog again(&model);
        // CONTROL: a fresh window is not born at the saved size.
        QVERIFY(again.size() != chosen);
        again.show();
        QVERIFY(QTest::qWaitForWindowExposed(&again));
        QTRY_COMPARE(again.size(), chosen);
        again.hide();
    }

    void geometrySavedByThePersistentDialogCarriesOver()
    {
        // PersistentDialog saved saveGeometry() as base64 under the same key;
        // that is what an existing install holds on its first canon open.
        const QSize legacy(720, 360);   // inside the 800x600 offscreen screen
        QByteArray blob;
        {
            QDialog old;
            old.resize(legacy);
            old.show();
            QVERIFY(QTest::qWaitForWindowExposed(&old));
            QCOMPARE(old.size(), legacy);
            blob = old.saveGeometry().toBase64();
            old.hide();
        }
        auto& settings = AppSettings::instance();
        settings.setValue(QLatin1String(kGeometryKey), QString::fromLatin1(blob));

        RadioModel model;
        MultiFlexDialog window(&model);
        // CONTROL: the window is not born at the legacy size.
        QVERIFY(window.size() != legacy);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_COMPARE(window.size(), legacy);
        window.hide();
        settings.remove(QLatin1String(kGeometryKey));
    }

    void enableButtonIsGreenOnAndRedOff()
    {
        auto& tm = ThemeManager::instance();
        const ThemeRestore restoreTheme;
        QStringList failures;
        for (const char* theme : {"Default Dark", "Default Light"}) {
            QVERIFY2(tm.setActiveTheme(QLatin1String(theme)), theme);
            RadioModel model;
            MultiFlexDialog window(&model);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QPushButton* button = findEnableButton(window);
            QVERIFY(button);
            const QColor success = tm.color(button, QStringLiteral("color.toggle.success.border.checked"));
            const QColor danger = tm.color(button, QStringLiteral("color.accent.danger"));
            // CONTROL: the two states must be told apart by color at all.
            QVERIFY(success != danger);

            auto measure = [&](bool on) {
                if (model.multiFlexEnabled() != on)
                    QTest::mouseClick(button, Qt::LeftButton);
                QCoreApplication::processEvents();
                const QImage image = button->grab().toImage();
                const QString state = on ? QStringLiteral("Enabled") : QStringLiteral("Disabled");
                if (button->text() != state)
                    failures << QStringLiteral("%1: button reads %2, not %3")
                                    .arg(QLatin1String(theme), button->text(), state);
                const int green = pixelsNear(image, success, 6);
                const int red = pixelsNear(image, danger, 6);
                qInfo().noquote() << QStringLiteral("BUTTON %1 %2: success %3, danger %4")
                                         .arg(QLatin1String(theme), state).arg(green).arg(red);
                const int want = on ? green : red;
                const int other = on ? red : green;
                if (want <= 0 || other != 0)
                    failures << QStringLiteral("%1 %2: success %3, danger %4")
                                    .arg(QLatin1String(theme), state).arg(green).arg(red);
            };
            measure(true);
            measure(false);
            measure(true);   // the property flip repolishes in both directions
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral("\n"))));
    }

    void returnInTheTableDoesNotToggleMultiFlex()
    {
        RadioModel model;
        MultiFlexDialog window(&model);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        addTwoStations(model);
        auto* table = window.findChild<QTableWidget*>();
        QPushButton* button = findEnableButton(window);
        QVERIFY(table && button);
        // CONTROL: the toggle is the dialog's default button, and a station
        // row is current, so the table passes Return up to the dialog.
        QVERIFY(button->isDefault());
        table->setFocus();
        table->setCurrentCell(1, 1);
        QTRY_VERIFY(table->hasFocus());
        const bool before = model.multiFlexEnabled();

        // Each key checked on its own: two clicks would toggle it back.
        QTest::keyClick(table, Qt::Key_Return);
        QCOMPARE(model.multiFlexEnabled(), before);
        QTest::keyClick(table, Qt::Key_Enter);
        QCOMPARE(model.multiFlexEnabled(), before);

        // CONTROL: the button itself still takes the keyboard when focused.
        button->setFocus();
        QTRY_VERIFY(button->hasFocus());
        QTest::keyClick(button, Qt::Key_Space);
        QCOMPARE(model.multiFlexEnabled(), !before);
    }

    void selectedRowsKeepTheirColors()
    {
        auto& tm = ThemeManager::instance();
        const ThemeRestore restoreTheme;
        QStringList failures;
        for (const char* theme : {"Default Dark", "Default Light"}) {
            QVERIFY2(tm.setActiveTheme(QLatin1String(theme)), theme);
            RadioModel model;
            MultiFlexDialog window(&model);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            addTwoStations(model);
            auto* table = window.findChild<QTableWidget*>();
            QVERIFY(table);
            const int ours = rowOfHandle(table, 0);
            const int other = rowOfHandle(table, 0x7594C952);
            QVERIFY(ours >= 0 && other >= 0);
            const QColor success = tm.color(table, QStringLiteral("color.accent.success"));
            const QColor cyan = tm.color(table, QStringLiteral("color.canon.cyan"));

            table->selectRow(other);
            QCoreApplication::processEvents();
            // CONTROL: the row really is selected, so the selection band and
            // its text color apply to it.
            QVERIFY(table->item(other, 0)->isSelected());
            const int tick = cellPixelsNear(table, other, 0, success, 40);

            table->selectRow(ours);
            QCoreApplication::processEvents();
            QVERIFY(table->item(ours, 1)->isSelected());
            const int station = cellPixelsNear(table, ours, 1, cyan, 40);

            qInfo().noquote() << QStringLiteral("SELECTED %1: tick %2, our station %3")
                                     .arg(QLatin1String(theme)).arg(tick).arg(station);
            if (tick <= 0)
                failures << QStringLiteral("%1: a selected row's tick is not green")
                                .arg(QLatin1String(theme));
            if (station <= 0)
                failures << QStringLiteral("%1: our selected station is not cyan")
                                .arg(QLatin1String(theme));
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral("\n"))));
    }

    void themeChangeRecolorsWithoutRebuildingRows()
    {
        auto& tm = ThemeManager::instance();
        const ThemeRestore restoreTheme;
        QVERIFY(tm.setActiveTheme(QStringLiteral("Default Dark")));
        RadioModel model;
        MultiFlexDialog window(&model);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        addTwoStations(model);
        auto* table = window.findChild<QTableWidget*>();
        QVERIFY(table);
        const int ours = rowOfHandle(table, 0);
        const int other = rowOfHandle(table, 0x7594C952);
        QWidget* disconnectBefore = table->cellWidget(other, 4);
        QTableWidgetItem* tickBefore = table->item(other, 0);
        QVERIFY(disconnectBefore && tickBefore);
        const QColor darkCyan = tm.color(table, QStringLiteral("color.canon.cyan"));

        QVERIFY(tm.setActiveTheme(QStringLiteral("Default Light")));
        QCoreApplication::processEvents();
        const QColor lightCyan = tm.color(table, QStringLiteral("color.canon.cyan"));
        // CONTROL: the two themes' cyan differ, so a recolor is observable.
        QVERIFY(darkCyan != lightCyan);
        QCOMPARE(table->item(ours, 1)->foreground().color(), lightCyan);
        QCOMPARE(table->item(other, 0)->foreground().color(),
                 tm.color(table, QStringLiteral("color.accent.success")));
        // The rows and their Disconnect buttons are the same objects.
        QCOMPARE(table->cellWidget(other, 4), disconnectBefore);
        QCOMPARE(table->item(other, 0), tickBefore);
    }

    void enableButtonHoverBrightensTheBorder()
    {
        auto& tm = ThemeManager::instance();
        const ThemeRestore restoreTheme;
        QStringList failures;
        for (const char* theme : {"Default Dark", "Default Light"}) {
            QVERIFY2(tm.setActiveTheme(QLatin1String(theme)), theme);
            RadioModel model;
            MultiFlexDialog window(&model);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QPushButton* button = findEnableButton(window);
            QVERIFY(button);
            const QColor aqua = tm.color(button, QStringLiteral("color.canon.aqua"));
            for (bool on : {true, false}) {
                if (model.multiFlexEnabled() != on)
                    QTest::mouseClick(button, Qt::LeftButton);
                QTest::mouseMove(&window, QPoint(4, window.height() - 4));
                QCoreApplication::processEvents();
                const int rest = pixelsNear(button->grab().toImage(), aqua, 6);
                QTest::mouseMove(button, button->rect().center());
                QCoreApplication::processEvents();
                const int hover = pixelsNear(button->grab().toImage(), aqua, 6);
                qInfo().noquote() << QStringLiteral("HOVER %1 %2: aqua at rest %3, hovered %4")
                                         .arg(QLatin1String(theme), button->text())
                                         .arg(rest).arg(hover);
                // CONTROL: no aqua at rest, so the hovered count is the border.
                if (rest != 0 || hover <= 0)
                    failures << QStringLiteral("%1 %2: aqua at rest %3, hovered %4")
                                    .arg(QLatin1String(theme), button->text())
                                    .arg(rest).arg(hover);
            }
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral("\n"))));
    }

    void closesWhenTheRadioDisconnects()
    {
        RadioModel model;
        MultiFlexDialog window(&model);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        // CONTROL: a connect does not close it.
        emit model.connectionStateChanged(true);
        QCoreApplication::processEvents();
        QVERIFY(window.isVisible());
        emit model.connectionStateChanged(false);
        QTRY_VERIFY(!window.isVisible());
    }

    void rowsCarryTheirTokenColors()
    {
        auto& tm = ThemeManager::instance();
        const ThemeRestore restoreTheme;
        QStringList failures;
        for (const char* theme : {"Default Dark", "Default Light"}) {
            QVERIFY2(tm.setActiveTheme(QLatin1String(theme)), theme);
            RadioModel model;
            MultiFlexDialog window(&model);
            QSignalSpy disconnects(&window, &MultiFlexDialog::disconnectClientRequested);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            addTwoStations(model);

            auto* table = window.findChild<QTableWidget*>();
            QVERIFY(table);
            // CONTROL: both stations are listed, so the per-row checks below
            // read real rows.
            QCOMPARE(table->rowCount(), 2);
            const QColor success = tm.color(table, QStringLiteral("color.accent.success"));
            const QColor cyan = tm.color(table, QStringLiteral("color.canon.cyan"));
            const QColor danger = tm.color(table, QStringLiteral("color.accent.danger"));
            if (qEnvironmentVariableIsSet("MULTIFLEX_TEST_GRAB_DIR"))
                window.grab().save(qEnvironmentVariable("MULTIFLEX_TEST_GRAB_DIR")
                                   + QStringLiteral("/rows-%1.png").arg(QLatin1String(theme)));

            for (int row = 0; row < table->rowCount(); ++row) {
                const bool isUs = table->item(row, 0)->data(Qt::UserRole).toUInt() == 0;
                const QString station = table->item(row, 1)->text();
                if (isUs) {
                    if (table->item(row, 1)->foreground().color() != cyan)
                        failures << QStringLiteral("%1: our station %2 is not canon cyan")
                                        .arg(QLatin1String(theme), station);
                    if (table->cellWidget(row, 4))
                        failures << QStringLiteral("%1: our own row offers Disconnect")
                                        .arg(QLatin1String(theme));
                    continue;
                }
                if (table->item(row, 0)->foreground().color() != success)
                    failures << QStringLiteral("%1: %2's PTT tick is not the success token")
                                    .arg(QLatin1String(theme), station);
                // The tick must take that color when drawn: a glyph that falls
                // back to a color-emoji font paints its own gray instead. The
                // row is not selected (ours is), so no selection color covers it.
                const QRect tickCell = table->visualItemRect(table->item(row, 0));
                const int green = pixelsNear(
                    table->viewport()->grab(tickCell).toImage(), success, 40);
                qInfo().noquote() << QStringLiteral("TICK %1: success %2")
                                         .arg(QLatin1String(theme)).arg(green);
                if (table->item(row, 0)->isSelected() || green <= 0)
                    failures << QStringLiteral("%1: %2's PTT tick is not drawn green")
                                    .arg(QLatin1String(theme), station);
                auto* button = qobject_cast<QPushButton*>(table->cellWidget(row, 4));
                if (!button) {
                    failures << QStringLiteral("%1: %2 has no Disconnect button")
                                    .arg(QLatin1String(theme), station);
                    continue;
                }
                const int red = pixelsNear(button->grab().toImage(), danger, 6);
                qInfo().noquote() << QStringLiteral("DISCONNECT %1: danger %2")
                                         .arg(QLatin1String(theme)).arg(red);
                if (red <= 0)
                    failures << QStringLiteral("%1: Disconnect is not drawn in the danger token")
                                    .arg(QLatin1String(theme));
                QTest::mouseClick(button, Qt::LeftButton);
                if (disconnects.count() != 1 || button->isEnabled())
                    failures << QStringLiteral("%1: Disconnect did not request once and disable")
                                    .arg(QLatin1String(theme));
            }
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral("\n"))));
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("multiflex-dashboard-window"));
    if (!profile.isValid())
        return 1;
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    MultiFlexDashboardWindowTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "multiflex_dashboard_window_test.moc"
