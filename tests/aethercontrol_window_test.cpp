// AetherControl is a canon workspace window.
//
// Pinned here, each with its control:
//   1. it is a CanonWindow workspace: an independent top-level window with no
//      native frame, titled AetherControl;
//   2. the size and place the operator gives it persist under the key it used
//      as a PersistentDialog (FlexControlDialogGeometry), so geometry saved
//      before the migration carries over, and a second window restores it;
//   3. Escape while the knob has the mouse captured releases the capture and
//      leaves the window open; the next Escape closes it (CanonWindow closes
//      on Escape, so the release has to win the first press);
//   4. the platform Close bindings close a captured knob's window;
//   5. the captured knob's ring is drawn in color.accent.success, under
//      Default Dark and Default Light.
//
// No radio and no serial port: the window is constructed bare, with a
// standalone slice so the knob is enabled.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/CanonWindow.h"
#include "gui/FlexControlDialog.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QDialog>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QtTest>

#include <cstdlib>
#include <memory>

using namespace AetherSDR;

namespace {

const char* const kGeometryKey = "FlexControlDialogGeometry";

QWidget* findKnob(QWidget& window)
{
    for (QWidget* w : window.findChildren<QWidget*>()) {
        if (w->accessibleName() == QLatin1String("FlexControl tuning knob"))
            return w;
    }
    return nullptr;
}

QLabel* findCaptureHint(QWidget& window)
{
    return window.findChild<QLabel*>(QStringLiteral("CaptureHint"));
}

bool armed(QWidget& window)
{
    QLabel* hint = findCaptureHint(window);
    return hint && hint->property("armed").toBool();
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

class AetherControlWindowTest : public QObject {
    Q_OBJECT
private slots:

    void isACanonWorkspace()
    {
        FlexControlDialog window;
        QVERIFY(qobject_cast<CanonWindow*>(&window));
        QCOMPARE(window.windowTitle(), QStringLiteral("AetherControl"));
        // A workspace is its own top-level window, not a dialog on its parent.
        QCOMPARE(window.windowType(), Qt::Window);
        QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
    }

    void geometryPersistsUnderTheOldKeyAndRestores()
    {
        auto& settings = AppSettings::instance();
        settings.remove(QLatin1String(kGeometryKey));
        // CONTROL: nothing saved yet, so the restore below can only come from
        // this slot's own close.
        QVERIFY(settings.value(QLatin1String(kGeometryKey)).toString().isEmpty());

        QSize chosen;
        {
            FlexControlDialog window;
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            chosen = window.minimumSize().expandedTo(QSize(640, 700)) + QSize(40, 0);
            window.resize(chosen);
            QCoreApplication::processEvents();
            QCOMPARE(window.size(), chosen);
            // Escape is the workspace's close: it hides the window and the
            // geometry is flushed on hide.
            QTest::keyClick(&window, Qt::Key_Escape);
            QTRY_VERIFY(!window.isVisible());
        }
        const QString saved = settings.value(QLatin1String(kGeometryKey)).toString();
        const QString suffix = QStringLiteral(",%1,%2").arg(chosen.width()).arg(chosen.height());
        QVERIFY2(saved.endsWith(suffix), qPrintable(saved));

        FlexControlDialog again;
        // CONTROL: a fresh window is not born at the saved size.
        QVERIFY(again.size() != chosen);
        again.show();
        QVERIFY(QTest::qWaitForWindowExposed(&again));
        QTRY_COMPARE(again.size(), chosen);
        again.hide();
    }

    void legacyBase64GeometryRestores()
    {
        auto& settings = AppSettings::instance();
        settings.remove(QLatin1String(kGeometryKey));
        FlexControlDialog window;
        const QSize chosen = window.minimumSize().expandedTo(QSize(640, 700)) + QSize(40, 0);
        QDialog legacy;
        legacy.resize(chosen);
        const QString saved = QString::fromLatin1(legacy.saveGeometry().toBase64());
        QVERIFY(!saved.contains(QLatin1Char(',')));
        settings.setValue(QLatin1String(kGeometryKey), saved);
        QVERIFY(window.size() != chosen);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_COMPARE(window.size(), chosen);
        window.hide();
        QVERIFY(settings.value(QLatin1String(kGeometryKey)).toString().contains(QLatin1Char(',')));
    }

    void closeShortcutClosesWhileCaptured_data()
    {
        QTest::addColumn<QKeySequence>("sequence");
        const QList<QKeySequence> bindings = QKeySequence::keyBindings(QKeySequence::Close);
        QVERIFY(!bindings.isEmpty());
        for (const QKeySequence& sequence : bindings) {
            QTest::newRow(qPrintable(sequence.toString())) << sequence;
        }
    }

    void closeShortcutClosesWhileCaptured()
    {
        QFETCH(QKeySequence, sequence);
        SliceModel slice(0);
        FlexControlDialog window;
        window.setSlice(&slice);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.activateWindow();
        QTRY_VERIFY(window.isActiveWindow());
        QWidget* knob = findKnob(window);
        QVERIFY(knob);
        QTest::mouseDClick(knob, Qt::LeftButton, Qt::NoModifier, knob->rect().center());
        QTRY_VERIFY(armed(window));
        QTest::keySequence(knob, sequence);
        QTRY_VERIFY2(!window.isVisible(), qPrintable(sequence.toString()));
        QVERIFY(!armed(window));
    }

    void escapeReleasesTheCaptureBeforeItCloses_data()
    {
        QTest::addColumn<QString>("target");
        QTest::newRow("window") << QStringLiteral("window");
        QTest::newRow("knob") << QStringLiteral("knob");
        QTest::newRow("button") << QStringLiteral("button");
    }

    void escapeReleasesTheCaptureBeforeItCloses()
    {
        QFETCH(QString, target);
        SliceModel slice(0);
        FlexControlDialog window;
        window.setSlice(&slice);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QWidget* knob = findKnob(window);
        QVERIFY2(knob, "the knob is found by its accessible name");
        QVERIFY2(knob->isEnabled(), "a slice enables the knob");
        QVERIFY2(findCaptureHint(window), "the capture hint is found");
        QVERIFY2(!armed(window), "the knob starts released");

        QTest::mouseDClick(knob, Qt::LeftButton, Qt::NoModifier, knob->rect().center());
        QTRY_VERIFY2(armed(window), "a double-click captures the mouse");

        QWidget* recipient = &window;
        if (target == QLatin1String("knob")) {
            recipient = knob;
        } else if (target == QLatin1String("button")) {
            recipient = window.findChild<QPushButton*>(QStringLiteral("PhysicalButton"));
            QVERIFY(recipient);
        }
        recipient->setFocus();
        QTest::keyClick(recipient, Qt::Key_Escape);
        QCoreApplication::processEvents();
        QVERIFY2(!armed(window), "the first Escape releases the capture");
        QVERIFY2(window.isVisible(), "the first Escape does not close the window");

        // CONTROL: released, Escape is the window's close again.
        QTest::keyClick(&window, Qt::Key_Escape);
        QTRY_VERIFY2(!window.isVisible(), "the next Escape closes the window");
    }

    void capturedRingIsTheSuccessToken()
    {
        auto& tm = ThemeManager::instance();
        const ThemeRestore restoreTheme;
        QStringList failures;
        for (const char* theme : {"Default Dark", "Default Light"}) {
            QVERIFY2(tm.setActiveTheme(QLatin1String(theme)), theme);
            SliceModel slice(0);
            FlexControlDialog window;
            window.setSlice(&slice);
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QWidget* knob = findKnob(window);
            QVERIFY(knob);
            const QColor success = tm.color(knob, QStringLiteral("color.accent.success"));

            const int released = pixelsNear(knob->grab().toImage(), success, 6);
            QTest::mouseDClick(knob, Qt::LeftButton, Qt::NoModifier, knob->rect().center());
            QTRY_VERIFY(armed(window));
            const int captured = pixelsNear(knob->grab().toImage(), success, 6);
            QTest::keyClick(knob, Qt::Key_Escape);
            QCoreApplication::processEvents();
            QVERIFY(!armed(window));

            qInfo().noquote() << QStringLiteral("RING %1: success-colored pixels released %2, captured %3")
                                     .arg(QLatin1String(theme)).arg(released).arg(captured);
            // CONTROL: the released knob shows none of it.
            if (released != 0)
                failures << QStringLiteral("%1: released knob has %2 success pixels")
                                .arg(QLatin1String(theme)).arg(released);
            if (captured <= 0)
                failures << QStringLiteral("%1: captured ring is not color.accent.success")
                                .arg(QLatin1String(theme));
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral("\n"))));
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aethercontrol-window"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    AetherControlWindowTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "aethercontrol_window_test.moc"
