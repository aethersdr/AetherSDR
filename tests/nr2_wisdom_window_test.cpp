// The FFTW Wisdom window NR2 shows on first use is a canon dialog.
//
// Pinned here, each with its control:
//   1. it is a non-modal CanonWindow tool window titled FFTW Wisdom, under the
//      canon/nr2Wisdom theme scope, that appears without taking the focus;
//   2. its sheet is canon: the progress chunk is color.brand.gradient, every
//      token resolves to a value in the window's scope and the old literal
//      ground is gone, under Default Dark and Default Light;
//   3. Cancel and Escape each request cancellation once, Cancel leaves the
//      window up showing the cancel state, and progress that arrives after a
//      cancel is ignored;
//   4. progress and step text from the planner reach the bar and status line,
//      and the activity line ticks with the elapsed time;
//   5. each outcome sets the bar, the activity line and the status line, and
//      the status line's accessible name carries the outcome.
//
// No planner runs: the test feeds the window what MainWindow's worker would.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"
#include "gui/CanonWindow.h"
#include "gui/Nr2WisdomDialog.h"

#include <QApplication>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QtTest>

using namespace AetherSDR;

namespace {

template <typename T>
T* child(QWidget& window, const char* name)
{
    return window.findChild<T*>(QLatin1String(name));
}

class ThemeRestore {
public:
    ThemeRestore() : m_theme(ThemeManager::instance().activeTheme()) {}
    ~ThemeRestore() { ThemeManager::instance().setActiveTheme(m_theme); }
    Q_DISABLE_COPY_MOVE(ThemeRestore)
private:
    const QString m_theme;
};

} // namespace

class Nr2WisdomWindowTest : public QObject {
    Q_OBJECT
private slots:

    void isACanonToolWindow()
    {
        Nr2WisdomDialog window;
        QVERIFY(qobject_cast<CanonWindow*>(&window));
        QCOMPARE(window.objectName(), QStringLiteral("nr2WisdomDialog"));
        QCOMPARE(window.windowTitle(), QStringLiteral("FFTW Wisdom"));
        QVERIFY(window.windowFlags().testFlag(Qt::FramelessWindowHint));
        QCOMPARE(window.windowType(), Qt::Tool);
        QCOMPARE(window.windowModality(), Qt::NonModal);
        QVERIFY(window.testAttribute(Qt::WA_ShowWithoutActivating));
#ifdef Q_OS_MACOS
        QVERIFY(window.testAttribute(Qt::WA_MacAlwaysShowToolWindow));
#endif
        QCOMPARE(window.property("themeContainer").toString(),
                 QStringLiteral("canon/nr2Wisdom"));
        bool header = false;
        for (QLabel* label : window.findChildren<QLabel*>()) {
            header = header || label->text() == QLatin1String("FFTW Wisdom");
        }
        QVERIFY2(header, "the canon header carries the title");
        // Every widget MainWindow and the screenshot recipe find by name.
        for (const char* name : {"nr2WisdomStatusLabel", "nr2WisdomActivityLabel",
                                 "nr2WisdomElapsedTimeLabel", "nr2WisdomReassuranceLabel"}) {
            QVERIFY2(child<QLabel>(window, name), name);
        }
        QVERIFY(child<QProgressBar>(window, "nr2WisdomProgressBar"));
        QVERIFY(child<QPushButton>(window, "nr2WisdomCancelButton"));
        // Return in the window must not press Cancel.
        QVERIFY(!child<QPushButton>(window, "nr2WisdomCancelButton")->autoDefault());
    }

    void sheetIsCanonInBothThemes()
    {
        ThemeRestore restore;
        auto& tm = ThemeManager::instance();
        for (const char* theme : {"Default Dark", "Default Light"}) {
            QVERIFY2(tm.setActiveTheme(QLatin1String(theme)), theme);
            Nr2WisdomDialog window;
            const QString sheet = window.bodyWidget()->styleSheet();
            QVERIFY2(sheet.contains(QLatin1String("QProgressBar::chunk")), theme);
            const int chunk = sheet.indexOf(QLatin1String("QProgressBar::chunk"));
            QVERIFY2(sheet.indexOf(QLatin1String("qlineargradient"), chunk) > chunk,
                     "the progress chunk is the brand gradient");
            // An unknown token resolves to "" and leaves a `color: ;` Qt drops,
            // so check each token the sheet uses resolves in the window's scope
            // and lands in the sheet, and that no declaration was left empty.
            for (const char* token :
                 {"color.canon.ink", "color.canon.inkSoft", "color.canon.nested",
                  "color.canon.line", "color.canon.lineHi", "color.canon.control",
                  "color.canon.cyan", "color.canon.aqua", "color.canon.muted",
                  "color.brand.gradient"}) {
                const QString value = tm.cssFragment(window.bodyWidget(), QLatin1String(token));
                QVERIFY2(!value.isEmpty(), token);
                QVERIFY2(sheet.contains(value), token);
            }
            QVERIFY2(!sheet.contains(QLatin1String(": ;")), "no token resolved to an empty value");
            QVERIFY2(!sheet.contains(QLatin1String("#050710")), "the old literal ground is gone");
        }
    }

    void cancelRequestsOnceAndShowsTheCancelState()
    {
        Nr2WisdomDialog window;
        window.show();
        QSignalSpy spy(&window, &Nr2WisdomDialog::cancelRequested);
        auto* cancel = child<QPushButton>(window, "nr2WisdomCancelButton");
        auto* bar = child<QProgressBar>(window, "nr2WisdomProgressBar");
        auto* status = child<QLabel>(window, "nr2WisdomStatusLabel");
        auto* reassurance = child<QLabel>(window, "nr2WisdomReassuranceLabel");

        // CONTROL: before the cancel, progress lands.
        window.setProgress(40);
        QCOMPARE(bar->value(), 40);

        QTest::mouseClick(cancel, Qt::LeftButton);
        QCOMPARE(spy.count(), 1);
        QVERIFY(window.isCancelRequested());
        QVERIFY(window.isVisible());
        QVERIFY(!cancel->isEnabled());
        QCOMPARE(bar->maximum(), 0);
        QVERIFY(status->text().startsWith(QLatin1String("Canceling FFTW wisdom generation")));
        QVERIFY(!reassurance->isHidden());
        QVERIFY(reassurance->text().startsWith(QLatin1String("Cancel requested")));

        // A second request (Escape) does not ask again.
        window.reject();
        QCOMPARE(spy.count(), 1);

        // The planner's queued reports after the cancel change nothing.
        window.setStepDescription(QStringLiteral("Computing REAL FORWARD FFT size 4096..."));
        window.setProgress(80);
        QVERIFY(status->text().startsWith(QLatin1String("Canceling FFTW wisdom generation")));
        QCOMPARE(bar->maximum(), 0);
        // A report would normally hide the reassurance line; after a cancel
        // the guard drops the report, so the cancel notice stays.
        QVERIFY(!reassurance->isHidden());
        QVERIFY(reassurance->text().startsWith(QLatin1String("Cancel requested")));
    }

    void escapeRequestsCancel()
    {
        Nr2WisdomDialog window;
        window.show();
        QSignalSpy spy(&window, &Nr2WisdomDialog::cancelRequested);
        QTest::keyClick(&window, Qt::Key_Escape);
        QCOMPARE(spy.count(), 1);
        QVERIFY(window.isCancelRequested());
    }

    void plannerReportsReachTheWindow()
    {
        Nr2WisdomDialog window;
        window.show();
        auto* bar = child<QProgressBar>(window, "nr2WisdomProgressBar");
        auto* status = child<QLabel>(window, "nr2WisdomStatusLabel");
        auto* activity = child<QLabel>(window, "nr2WisdomActivityLabel");
        auto* elapsed = child<QLabel>(window, "nr2WisdomElapsedTimeLabel");

        window.setStepDescription(QStringLiteral("Computing COMPLEX FORWARD FFT size 2048..."));
        QVERIFY(status->text().startsWith(QLatin1String("Computing COMPLEX FORWARD FFT size 2048...")));
        QVERIFY(status->text().endsWith(QLatin1String("close when wisdom generation is complete.")));
        window.setProgress(38);
        QCOMPARE(bar->value(), 38);

        // The activity line ticks every 500 ms.
        QTRY_VERIFY_WITH_TIMEOUT(activity->text().startsWith(QLatin1String("Working.")), 2000);
        QVERIFY2(QRegularExpression(QStringLiteral("^Elapsed: \\d+:[0-5]\\d$"))
                     .match(elapsed->text())
                     .hasMatch(),
                 qPrintable(elapsed->text()));
    }

    void outcomesSetTheWindow()
    {
        struct Case {
            Nr2WisdomDialog::Outcome outcome;
            int value;
            const char* activity;
            const char* status;
        };
        const Case cases[] = {
            {Nr2WisdomDialog::Outcome::Complete, 100, "Complete", "Wisdom generation complete!"},
            {Nr2WisdomDialog::Outcome::Canceled, 0, "Stopped",
             "Wisdom generation canceled. Audio was left unchanged."},
            {Nr2WisdomDialog::Outcome::Failed, 0, "Stopped",
             "Wisdom generation failed. Audio was left unchanged."},
        };
        for (const Case& c : cases) {
            Nr2WisdomDialog window;
            window.setProgress(50);
            window.showResult(c.outcome);
            auto* bar = child<QProgressBar>(window, "nr2WisdomProgressBar");
            auto* status = child<QLabel>(window, "nr2WisdomStatusLabel");
            QCOMPARE(bar->maximum(), 100);
            QCOMPARE(bar->value(), c.value);
            QCOMPARE(child<QLabel>(window, "nr2WisdomActivityLabel")->text(),
                     QLatin1String(c.activity));
            QCOMPARE(status->text(), QLatin1String(c.status));
            QCOMPARE(status->accessibleName(), QLatin1String(c.status));
            QVERIFY(child<QLabel>(window, "nr2WisdomReassuranceLabel")->isHidden());
            // The activity line has stopped: it no longer ticks.
            QTest::qWait(600);
            QCOMPARE(child<QLabel>(window, "nr2WisdomActivityLabel")->text(),
                     QLatin1String(c.activity));
        }
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("nr2-wisdom-window"));
    if (!profile.isValid())
        return 1;
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();
    Nr2WisdomWindowTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "nr2_wisdom_window_test.moc"
