// Construction smoke test for the System Info dialog (#2554).
//
// Written because the dialog shipped with a null dereference that a full build
// and 326 passing tests did not catch: rebuildCategoryFilters() reached through
// m_logViewer for a parent widget before m_logViewer existed, so the dialog
// crashed the instant it was constructed. Nothing in the suite constructed it.
//
// This is deliberately shallow — construct, show, hide, destroy — because that
// is the path that was broken. Depth can come later; coverage of the path an
// operator takes by clicking the menu item cannot.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/LogManager.h"
#include "core/ThreadCpuRing.h"
#include "gui/SparklineDelegate.h"
#include "gui/SystemInfoDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTemporaryDir>
#include <QPainter>
#include <QPixmap>
#include <QTabWidget>
#include <QTableWidget>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void report(const char* what, bool ok)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    TestSettingsProfile profile(QStringLiteral("aether-system-info-dialog-test"));

    // Construction alone is the regression this guards.
    SystemInfoDialog dialog;
    report("the dialog constructs without crashing", true);

    auto* tabs = dialog.findChild<QTabWidget*>();
    report("it has a tab widget", tabs != nullptr);
    if (tabs != nullptr) {
        report("it has exactly two tabs", tabs->count() == 2);
        report("first tab is Threads", tabs->tabText(0) == QLatin1String("Threads"));
        report("second tab is Logs", tabs->tabText(1) == QLatin1String("Logs"));
    }

    auto* table = dialog.findChild<QTableWidget*>();
    report("the thread table exists", table != nullptr);
    if (table != nullptr) {
        // Four data columns plus a trailing spacer that absorbs slack on a wide
        // window — assert the shape, not a magic number, so a future column is a
        // deliberate edit here rather than a silent count change.
        report("the table has seven data columns plus a spacer", table->columnCount() == 8);
        report("column 0 is Thread",
               table->horizontalHeaderItem(0)->text() == QLatin1String("Thread"));
        report("column 1 is TID",
               table->horizontalHeaderItem(1)->text() == QLatin1String("TID"));
        report("column 2 is State",
               table->horizontalHeaderItem(2)->text() == QLatin1String("State"));
        report("column 3 is CPU %",
               table->horizontalHeaderItem(3)->text() == QLatin1String("CPU %"));
        report("column 4 is Peak 60 s",
               table->horizontalHeaderItem(4)->text() == QLatin1String("Peak 60 s"));
        report("column 5 is Total CPU (s)",
               table->horizontalHeaderItem(5)->text() == QLatin1String("Total CPU (s)"));
        report("column 6 is the sparkline",
               table->horizontalHeaderItem(6)->text() == QLatin1String("Last 60 s"));
        report("the last column is an unlabelled spacer",
               table->horizontalHeaderItem(7)->text().isEmpty());
        // Ragged decimals: the items hold real doubles so the column sorts
        // numerically, but a double that rounds to 16.0 renders as "16" next to
        // a neighbour's "3.2". displayText() formats without touching the value
        // the sort compares.
        for (const int column : {3, 4, 5}) {
            auto* styled = dynamic_cast<QStyledItemDelegate*>(
                table->itemDelegateForColumn(column));
            // 16.0 is the case that motivated this: stored as a double it
            // renders as "16" beside a neighbour's "3.2". 3.24 pins that a
            // value is rounded rather than truncated to its first digit.
            // Deliberately NOT a .x5 half-way value — which way that rounds is
            // the C++ library's business, not this code's, and asserting it
            // would be testing the platform.
            const bool formats = styled != nullptr
                && styled->displayText(QVariant(16.0), QLocale::c()) == QLatin1String("16.0")
                && styled->displayText(QVariant(3.24), QLocale::c()) == QLatin1String("3.2")
                && styled->displayText(QVariant(0.0), QLocale::c()) == QLatin1String("0.0");
            report(column == 3 ? "CPU % always shows one decimal"
                   : column == 4 ? "Peak always shows one decimal"
                                 : "Total CPU always shows one decimal", formats);
        }
        // TID is a count, not a measurement — it must NOT gain a decimal.
        report("TID keeps no decimal",
               dynamic_cast<QStyledItemDelegate*>(table->itemDelegateForColumn(1)) == nullptr
                   || dynamic_cast<QStyledItemDelegate*>(table->itemDelegateForColumn(1))
                          ->displayText(QVariant(qulonglong(243838)), QLocale::c())
                      != QLatin1String("243838.0"));

        report("the sparkline column has its own delegate",
               dynamic_cast<SparklineDelegate*>(table->itemDelegateForColumn(6)) != nullptr);
        report("the table is sortable", table->isSortingEnabled());
    }

    // show() starts the collector thread and opens the log tail; hide() must
    // stop and close them. Both run in showEvent/hideEvent, which is where a
    // teardown mistake would surface as a hang or a crash on close.
    dialog.show();
    QCoreApplication::processEvents();
    report("show() does not crash", true);

    dialog.hide();
    QCoreApplication::processEvents();
    report("hide() stops sampling cleanly", true);

    // ── A sample driven all the way into the table ───────────────────────────
    //
    // Every defect this dialog shipped with was found by opening it, not by the
    // suite, and the table's contents were the largest thing no test could
    // reach. applySample is a slot so this can reach it without a collector, a
    // worker thread, or a machine busy enough to produce an interesting row.
    qRegisterMetaType<QVector<AetherSDR::ThreadCpuSample>>(
        "QVector<AetherSDR::ThreadCpuSample>");

    const auto drive = [&dialog](const QVector<ThreadCpuSample>& samples) {
        return QMetaObject::invokeMethod(
            &dialog, "applySample", Qt::DirectConnection,
            Q_ARG(QVector<AetherSDR::ThreadCpuSample>, samples));
    };

    ThreadCpuSample hot;
    hot.tid = 4242;
    hot.name = QStringLiteral("AudioEngine");
    hot.cpuUsecs = 2500000;
    hot.cpuPercentOfCore = 91.5;
    hot.state = ThreadRunState::Running;

    ThreadCpuSample idle;
    idle.tid = 4243;                 // deliberately unnamed: the "(unnamed)" rule
    idle.cpuPercentOfCore = 0.0;
    idle.state = ThreadRunState::Unknown;   // what every Windows row will read

    report("a sample can be driven into the dialog", drive({hot, idle}));

    if (table != nullptr) {
        report("both threads land as rows", table->rowCount() == 2);

        // Sorted CPU-descending, so the hot thread is row 0. Asserting the
        // ORDER as well as the contents is the point: the numeric columns are
        // filled with setData rather than setText precisely so that 9 % does
        // not sort above 80 %.
        report("the hot thread sorts to the top",
               table->item(0, 0) != nullptr
                   && table->item(0, 0)->text() == QLatin1String("AudioEngine"));
        report("an unnamed thread reads as (unnamed), not as a blank cell",
               table->item(1, 0) != nullptr
                   && table->item(1, 0)->text() == QLatin1String("(unnamed)"));
        report("a known state reads as a word",
               table->item(0, 2) != nullptr
                   && table->item(0, 2)->text() == QLatin1String("Running"));
        // The Windows cell, in effect: a platform that cannot report state
        // shows a dash rather than a value derived from something else.
        report("an unknown state reads as a dash, not a guess",
               table->item(1, 2) != nullptr
                   && table->item(1, 2)->text() == QString::fromUtf8("—"));
        report("CPU % lands in its column",
               table->item(0, 3) != nullptr
                   && qAbs(table->item(0, 3)->data(Qt::DisplayRole).toDouble() - 91.5) < 0.05);
        report("Peak matches the only reading so far",
               table->item(0, 4) != nullptr
                   && qAbs(table->item(0, 4)->data(Qt::DisplayRole).toDouble() - 91.5) < 0.05);

        // Peak is a HIGH-water mark: the falling CPU % must not drag it down.
        ThreadCpuSample cooled = hot;
        cooled.cpuPercentOfCore = 3.0;
        drive({cooled, idle});
        report("CPU % follows the newest reading down",
               table->item(0, 3) != nullptr
                   && qAbs(table->item(0, 3)->data(Qt::DisplayRole).toDouble() - 3.0) < 0.05);
        report("Peak holds the earlier spike after CPU % falls",
               table->item(0, 4) != nullptr
                   && qAbs(table->item(0, 4)->data(Qt::DisplayRole).toDouble() - 91.5) < 0.05);

        // The delegate draws from this role, so it has to survive the trip
        // through QVariant intact and in order — oldest first.
        const QList<double> series =
            table->item(0, 6)->data(SparklineDelegate::kSeriesRole).value<QList<double>>();
        report("the sparkline series round-trips through the item role",
               series.size() == 2 && qAbs(series.first() - 91.5) < 0.05
                   && qAbs(series.last() - 3.0) < 0.05);
        report("the sparkline column sorts by peak",
               qAbs(table->item(0, 6)->data(Qt::DisplayRole).toDouble() - 91.5) < 0.05);

        // Painting is where a delegate crashes, and the two cases that reach
        // the awkward code are the ones a live dialog hits first: a thread seen
        // for the very first time, and one with a single reading — a polyline
        // of one point draws nothing, so it is handled separately.
        SparklineDelegate delegate;
        QPixmap canvas(140, 20);
        canvas.fill(Qt::black);
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 140, 20);
        {
            QPainter painter(&canvas);
            delegate.paint(&painter, option, table->model()->index(0, 6));
            delegate.paint(&painter, option, table->model()->index(1, 6));
        }
        report("painting a populated and an empty series does not crash", true);

        // A degenerate cell — zero height and width — must not divide by a
        // zero span. It happens while a column is being dragged closed.
        {
            QPainter painter(&canvas);
            QStyleOptionViewItem collapsed;
            collapsed.rect = QRect(0, 0, 0, 0);
            delegate.paint(&painter, collapsed, table->model()->index(0, 6));
        }
        report("painting into a collapsed cell does not crash", true);
    }

    // ── The threshold alert ──────────────────────────────────────────────────
    //
    // Acceptance criterion 3, minimal form. Driven directly rather than by
    // saturating a core, which is neither reproducible nor kind to a CI box.
    if (auto* summary = dialog.findChild<QLabel*>(
            QStringLiteral("systemInfoThreadSummary"))) {
        report("no alert while the busiest thread is below the line",
               summary->toolTip().isEmpty());

        QMetaObject::invokeMethod(&dialog, "onThresholdExceeded", Qt::DirectConnection,
                                  Q_ARG(QString, QStringLiteral("AudioEngine")),
                                  Q_ARG(double, 95.0));
        report("crossing the threshold raises the alert",
               summary->toolTip().contains(QLatin1String("AudioEngine")));

        // The crossing signal is edge-triggered, so a thread that stays hot
        // sends nothing further — the alert must survive the samples that
        // arrive while it is still above the line.
        ThreadCpuSample stillHot = hot;
        stillHot.cpuPercentOfCore = 93.0;
        drive({stillHot, idle});
        report("the alert survives a sample that is still above the line",
               !summary->toolTip().isEmpty());

        // And nothing announces coming back down, so clearing is the dialog's
        // job. This is the half an edge-triggered signal cannot do for it.
        ThreadCpuSample cooled = hot;
        cooled.cpuPercentOfCore = 4.0;
        drive({cooled, idle});
        report("the alert clears once the busiest thread drops below the line",
               summary->toolTip().isEmpty());

        // A red line over a table that has stopped updating would claim a
        // thread is saturating a core right now, when nothing is being measured.
        QMetaObject::invokeMethod(&dialog, "onThresholdExceeded", Qt::DirectConnection,
                                  Q_ARG(QString, QStringLiteral("AudioEngine")),
                                  Q_ARG(double, 99.0));
        dialog.show();
        QCoreApplication::processEvents();
        dialog.hide();
        QCoreApplication::processEvents();
        report("hiding the dialog clears a standing alert",
               summary->toolTip().isEmpty());
    } else {
        report("the thread summary label is addressable by name", false);
    }

    // ── Logs tab: the two classes of line the filter used to hide ────────────
    //
    // Both were found by re-reading LogManager rather than by running the
    // dialog, which is why they are pinned here: neither shows up as a crash or
    // a failing build, only as a line that quietly never appears.
    {
        auto* viewer = dialog.findChild<QPlainTextEdit*>(
            QStringLiteral("systemInfoLogViewer"));
        report("the log viewer is addressable by name", viewer != nullptr);

        const auto box = [&dialog](const char* id) {
            return dialog.findChild<QCheckBox*>(
                QStringLiteral("logFilter_%1").arg(QLatin1String(id)));
        };
        const auto feed = [&dialog](const QString& line) {
            QMetaObject::invokeMethod(&dialog, "appendLogLine", Qt::DirectConnection,
                                      Q_ARG(QString, line));
        };

        // Scope: the three categories the issue names for this tab, and no
        // others. Threads enumerates every thread; the log answers what the
        // perf subsystem was doing. Offering the whole registry here would make
        // the tab a duplicate of the network dialog's log viewer.
        report("aether.perf is offered", box("aether.perf") != nullptr);
        report("aether.render is offered", box("aether.render") != nullptr);
        report("aether.audio is offered", box("aether.audio") != nullptr);
        report("a category outside the perf set is NOT offered here",
               box("aether.cw") == nullptr && box("aether.dax") == nullptr);
        report("all three start ticked",
               box("aether.perf")->isChecked() && box("aether.render")->isChecked()
                   && box("aether.audio")->isChecked());

        // The defect this tab shipped with, on the categories it actually
        // offers: a category switched OFF still writes warnings and criticals
        // (LogManager.h:58). The row used to skip those categories entirely, so
        // the most important lines in the file had no box that could show them
        // — and aether.perf is switched off by default, so this is the common
        // case rather than an edge one.
        const bool perfOff = !LogManager::instance().isEnabled(QStringLiteral("aether.perf"));
        report("aether.perf is switched off by default, so this case is real", perfOff);
        if (viewer != nullptr) {
            feed(QStringLiteral("[00:00:01.000] WRN aether.perf: frame budget exceeded"));
            report("a warnings-only category's warning is shown, not filtered away",
                   viewer->toPlainText().contains(QLatin1String("frame budget")));
            box("aether.perf")->setChecked(false);
            report("unticking it hides the line again",
                   !viewer->toPlainText().contains(QLatin1String("frame budget")));
            box("aether.perf")->setChecked(true);
        }

        // Out of scope by design, and asserted so the decision is visible in
        // the suite rather than only in a commit message: uncategorized output
        // has no box here, so it does not appear.
        report("uncategorized output is not offered on this tab",
               box("default") == nullptr);
        if (viewer != nullptr) {
            feed(QStringLiteral("[00:00:02.000] WRN default: uncategorized warning"));
            report("and does not reach the view",
                   !viewer->toPlainText().contains(QLatin1String("uncategorized warning")));
        }

        // ── Follow-live ──────────────────────────────────────────────────────
        //
        // Promised on the issue on 2026-08-25 and not built: the view scrolled
        // itself to the newest line on every append, so a stall could not be
        // read without the log yanking itself away every 500 ms.
        auto* live = dialog.findChild<QPushButton*>(
            QStringLiteral("systemInfoLogLiveToggle"));
        report("the Logs tab has a Live toggle", live != nullptr);
        if (live != nullptr && viewer != nullptr) {
            report("it starts following", live->isChecked()
                       && live->text() == QLatin1String("Live"));

            live->setChecked(false);
            report("turning it off reads as Paused",
                   live->text() == QLatin1String("Paused"));

            feed(QStringLiteral("[00:00:03.000] WRN aether.perf: arrived while paused"));
            report("a line arriving while paused stays out of the view",
                   !viewer->toPlainText().contains(QLatin1String("while paused")));

            // Retained, not dropped: resuming must show what was missed rather
            // than picking up from wherever the file has reached.
            live->setChecked(true);
            report("resuming catches up on what arrived while paused",
                   viewer->toPlainText().contains(QLatin1String("while paused")));

            // Scrolling up is the pause gesture, so it has to disengage
            // following without the button being touched.
            dialog.show();
            QCoreApplication::processEvents();
            viewer->setFixedHeight(40);
            for (int i = 0; i < 200; ++i) {
                feed(QStringLiteral("[00:00:04.%1] WRN aether.perf: filler line %2")
                         .arg(i, 3, 10, QLatin1Char('0')).arg(i));
            }
            QCoreApplication::processEvents();
            if (viewer->verticalScrollBar()->maximum() > 0) {
                viewer->verticalScrollBar()->setValue(0);
                QCoreApplication::processEvents();
                report("scrolling up turns following off by itself",
                       !live->isChecked() && live->text() == QLatin1String("Paused"));
            } else {
                report("the viewer became scrollable so the gesture can be tested",
                       false);
            }

            // And our OWN jump to the bottom must not read as that gesture, or
            // the first appended line would switch following off.
            live->setChecked(true);
            feed(QStringLiteral("[00:00:05.000] WRN aether.perf: still following"));
            QCoreApplication::processEvents();
            report("appending while live does not switch following off",
                   live->isChecked());
            dialog.hide();
            QCoreApplication::processEvents();
        }
    }

    // ── The tail survives the file moving underneath it ──────────────────────
    //
    // A tail running for hours outlives log rotation. Reading on from a stale
    // handle looks exactly like a log that stopped: the pane goes quiet and
    // nothing says why, which during an investigation reads as "the app stopped
    // logging" rather than "this view is stuck".
    {
        QTemporaryDir tempDir;
        report("a temporary log directory is available", tempDir.isValid());
        const QString logPath = tempDir.filePath(QStringLiteral("aethersdr.log"));
        // Seeded AFTER startLogging, which opens the file for writing and
        // truncates it — a line written before that call does not survive to be
        // tailed.
        LogManager::instance().startLogging(logPath, false);
        {
            QFile seed(logPath);
            seed.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
            seed.write("[00:00:00.000] WRN aether.perf: before the reset\n");
        }

        SystemInfoDialog tailing;
        tailing.show();
        QCoreApplication::processEvents();

        auto* path = tailing.findChild<QLabel*>(QStringLiteral("systemInfoLogPath"));
        report("the Logs tab names the file it is following",
               path != nullptr && path->text().contains(logPath));

        auto* view = tailing.findChild<QPlainTextEdit*>(
            QStringLiteral("systemInfoLogViewer"));
        report("the seeded line is tailed",
               view != nullptr
                   && view->toPlainText().contains(QLatin1String("before the reset")));

        // A temporary hide pauses observation but does not start a new tail.
        // Reopening from the last 64 KiB while retaining the existing model
        // duplicated every recent line on each frameless-mode toggle.
        tailing.hide();
        QCoreApplication::processEvents();
        tailing.show();
        QCoreApplication::processEvents();
        report("hide/show does not duplicate the retained log tail",
               view != nullptr
                   && view->toPlainText().count(
                          QLatin1String("before the reset")) == 1);

        // Rotation: the file is replaced by a shorter one at the same path, so
        // the handle's position is now past its end.
        {
            QFile rotated(logPath);
            rotated.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
            rotated.write("[00:00:01.000] WRN aether.perf: after the reset\n");
        }
        QMetaObject::invokeMethod(&tailing, "pollLog", Qt::DirectConnection);
        QCoreApplication::processEvents();

        if (view != nullptr) {
            report("lines written after a reset are picked up",
                   view->toPlainText().contains(QLatin1String("after the reset")));
            // The notice rides a reserved category with no checkbox, so it is
            // visible even though "default" is unticked by default.
            report("the reset is announced rather than passing silently",
                   view->toPlainText().contains(QLatin1String("Log file was reset")));
        }

        report("the dialog has a Close button",
               tailing.findChild<QPushButton*>(
                   QStringLiteral("systemInfoCloseButton")) != nullptr);

        tailing.hide();
        QCoreApplication::processEvents();
        LogManager::instance().shutdownLogging();
    }

    std::printf("%s\n", g_failures == 0 ? "system_info_dialog_test: all passed"
                                        : "system_info_dialog_test: FAILURES");
    return g_failures == 0 ? 0 : 1;
}
