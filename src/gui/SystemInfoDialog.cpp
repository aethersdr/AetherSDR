#include "SystemInfoDialog.h"

#include "LogSyntaxHighlighter.h"
#include "SparklineDelegate.h"
#include "TimeSeriesGraphWidget.h"
#include "core/LogManager.h"
#include "core/ThemeManager.h"

#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QLocale>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QScrollBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

// The gui-side ring repeats the collector's cadence rather than including the
// core header (aetherd touchpoint burndown); this is where the two are pinned.
static_assert(AetherSDR::MemoryHistoryRing::kSampleIntervalMs
                  == AetherSDR::SystemInfoCollector::kSampleIntervalMs,
              "MemoryHistoryRing::kSampleIntervalMs must match the collector's cadence");

namespace AetherSDR {

namespace {

constexpr int kLogPollMs = 500;

// The categories the issue names for this tab: "Live tail of perf-related
// logging categories: lcPerf, lcRender, lcAudio."
//
// Three, and not the whole registry. The two tabs are deliberately scoped
// differently: Threads enumerates EVERY thread in the process because the
// failure it exists to catch is one of them saturating a core (#2545), while
// the log answers what the perf subsystem was doing. Widening this to all
// twenty-eight categories would make the tab a duplicate of the log viewer in
// NetworkDiagnosticsDialog, which is the doubt the issue's own analysis raised
// about it.
const char* const kPerfCategories[] = {"aether.perf", "aether.render", "aether.audio"};

// The tab's own notices — currently just "the log was reset" — ride the same
// path as real log lines so that replaying the buffer keeps them in place. They
// get no checkbox and are permanently visible: a notice explaining why the pane
// just emptied is useless if it lands behind a filter the operator has not
// ticked, and after this commit "default" is unticked by default.
const char* const kNoticeCategory = "systeminfo";
constexpr qint64 kInitialTailBytes = 64 * 1024;
constexpr qsizetype kMaxStoredLines = 5000;

// Overview card bands, verbatim from the issue body's card table (#2554:
// "CPU Total … yellow ≥50%, red ≥80%", "Max Thread … yellow ≥70%, red ≥90%",
// "Memory … yellow ≥1 GB, red ≥2 GB"). Starting values, not findings: nobody
// has measured a normal session against them, which each card's tooltip says.
// The CPU pair is also what the status bar's own label uses (MainWindow.cpp).
// The issue's fourth card, "GUI Frame Rate" with yellow <25 Hz / red <15 Hz,
// is not built: the perf heartbeat it would be sourced from is a 20 Hz timer,
// so a healthy app would sit in the yellow band by construction. That card
// reads the heartbeat's tick lag instead, uncoloured, until a maintainer sets
// bands for it (plan §12.2, D1).
constexpr double kCpuWarnPercent = 50.0;
constexpr double kCpuDangerPercent = 80.0;
constexpr double kMaxThreadWarnPercent = 70.0;
constexpr double kMaxThreadDangerPercent = 90.0;
constexpr double kMemoryWarnMb = 1024.0;
constexpr double kMemoryDangerMb = 2048.0;

// A status card in the network dialog's shape — title, large value, caption —
// built from theme tokens rather than that dialog's literal gradient, because
// the hardcoded-colour ratchet counts literals and this dialog has none.
struct OverviewCard {
    QFrame* frame{nullptr};
    QLabel* value{nullptr};
    QLabel* caption{nullptr};
};

OverviewCard makeOverviewCard(const QString& title, const QString& caption,
                              const QString& objectName, const QString& accessible,
                              const QString& tip, QWidget* parent)
{
    OverviewCard card;
    card.frame = new QFrame(parent);
    card.frame->setObjectName(QStringLiteral("systemInfoCard"));
    card.frame->setAttribute(Qt::WA_StyledBackground, true);
    card.frame->setMinimumHeight(96);
    ThemeManager::instance().applyStyleSheet(
        card.frame,
        QStringLiteral("QFrame#systemInfoCard { background: {{color.background.1}}; "
                       "border: 1px solid {{color.background.2}}; border-radius: 7px; }"));
    auto* layout = new QVBoxLayout(card.frame);
    layout->setContentsMargins(8, 5, 8, 8);
    layout->setSpacing(4);
    // Every label says background: transparent — the frame's stylesheet
    // background would otherwise be painted again behind each child, as a
    // darker box around the text (found on the demo, 2026-09-04). The network
    // dialog's own stylesheet carries the same rule for the same reason.
    auto* titleLabel = new QLabel(title, card.frame);
    ThemeManager::instance().applyStyleSheet(
        titleLabel, QStringLiteral("QLabel { color: {{color.text.secondary}}; font-weight: 600; "
                                   "background: transparent; }"));
    card.value = new QLabel(QStringLiteral("—"), card.frame);
    card.value->setObjectName(objectName);
    card.value->setAccessibleName(accessible);
    card.value->setToolTip(tip);
    card.value->setMinimumHeight(card.value->fontMetrics().height() + 4);
    card.caption = new QLabel(caption, card.frame);
    card.caption->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(
        card.caption, QStringLiteral("QLabel { color: {{color.text.secondary}}; font-size: 11px; "
                                     "background: transparent; }"));
    layout->addWidget(titleLabel);
    layout->addWidget(card.value);
    layout->addWidget(card.caption);
    layout->addStretch(1);
    return card;
}

// One top-threads line per legend entry: a thread with no kernel name
// reads "(unnamed)", and two members sharing a name get their tid appended so
// the legend can tell them apart and clicking one selects one.
QString threadLabel(const CpuHistoryRing::ThreadSeries& series, bool nameShared)
{
    const QString base = series.name.isEmpty() ? QStringLiteral("(unnamed)") : series.name;
    return nameShared ? QStringLiteral("%1 (%2)").arg(base).arg(series.tid) : base;
}

// A trailing spacer column soaks up slack on a wide window. Without it the
// stretch has to land on a real column, and the thread name — the only one
// that varies — ends up several hundred pixels wider than the longest name.
// One decimal on every numeric column, always.
//
// The items store real doubles, because that is what makes the table sort
// numerically — 9 % must not sort above 80 %, which it would as text. But a
// double displays through its own default formatting, so a value that rounds to
// 16.0 renders as "16" while 3.2 renders as "3.2", and a right-aligned column
// ends up with ragged decimals.
//
// displayText() is the hook for exactly this: it changes what is drawn without
// touching the value underneath, so sorting still compares numbers.
class OneDecimalDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QString displayText(const QVariant& value, const QLocale& locale) const override
    {
        if (value.typeId() == QMetaType::Double || value.typeId() == QMetaType::Float) {
            return locale.toString(value.toDouble(), 'f', 1);
        }
        return QStyledItemDelegate::displayText(value, locale);
    }
};

// An unnamed row is a raw std::thread Qt never saw — say so rather than leaving
// a blank cell that reads as a rendering fault. Shared so the table cell and the
// summary line cannot describe the same thread differently.
QString displayName(const ThreadCpuSample& sample)
{
    return sample.name.isEmpty() ? QStringLiteral("(unnamed)") : sample.name;
}

// Column order follows the issue's own list for the Threads tab.
enum Column { ColName = 0, ColTid, ColState, ColCpu, ColPeak, ColTotal, ColSpark,
              ColSpacer, ColumnCount };

// One vocabulary for a column two kernels describe differently. macOS reports
// TH_STATE_* and Linux a single character in /proc; Windows reports nothing at
// all, which is a dash rather than a guess — there is no documented per-thread
// state on THREADENTRY32 or GetThreadTimes.
QString stateText(ThreadRunState state)
{
    switch (state) {
    case ThreadRunState::Running:         return QStringLiteral("Running");
    case ThreadRunState::Waiting:         return QStringLiteral("Waiting");
    case ThreadRunState::Uninterruptible: return QStringLiteral("Uninterruptible");
    case ThreadRunState::Stopped:         return QStringLiteral("Stopped");
    case ThreadRunState::Halted:          return QStringLiteral("Halted");
    case ThreadRunState::Zombie:          return QStringLiteral("Zombie");
    case ThreadRunState::Unknown:         break;
    }
    return QStringLiteral("—");
}

}  // namespace

SystemInfoDialog::SystemInfoDialog(MemoryHistoryRing* history, CpuHistoryRing* cpuHistory,
                                   UiTickLagMeter* tickLagMeter, QWidget* parent)
    // Title tracks the menu entry — see MainWindow_Menus.cpp for why it is not
    // "System Info". The geometry KEY deliberately does not follow: it is a
    // settings id, and changing it would silently discard the saved window
    // position of anyone who has already used this dialog. Same rule LogManager
    // applies to category ids.
    : PersistentDialog(QStringLiteral("Runtime Monitor"),
                       QStringLiteral("SystemInfoDialogGeometry"), parent)
{
    if (history != nullptr) {
        m_memoryRing = history;
    }
    if (cpuHistory != nullptr) {
        m_cpuRing = cpuHistory;
    }
    if (tickLagMeter != nullptr) {
        m_tickLagMeter = tickLagMeter;
    }
    auto* layout = new QVBoxLayout(bodyWidget());
    auto* tabs = new QTabWidget(bodyWidget());
    // The issue's order: Overview / Threads / Memory / (Painters) / Logs.
    tabs->addTab(buildOverviewTab(), QStringLiteral("Overview"));
    tabs->addTab(buildThreadsTab(), QStringLiteral("Threads"));
    tabs->addTab(buildMemoryTab(), QStringLiteral("Memory"));
    tabs->addTab(buildLogsTab(), QStringLiteral("Logs"));
    layout->addWidget(tabs);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    auto* closeButton = new QPushButton(QStringLiteral("Close"), bodyWidget());
    closeButton->setObjectName(QStringLiteral("systemInfoCloseButton"));
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    buttonRow->addWidget(closeButton);
    layout->addLayout(buttonRow);

    // A reopened dialog shows the history it was handed before the first new
    // sample arrives; with an empty ring this is a no-op.
    refreshMemoryChart();
    refreshOverview();

    resize(900, 600);
}

SystemInfoDialog::~SystemInfoDialog()
{
    stopSampling();
    pauseLogTail();
}

// ── Threads ─────────────────────────────────────────────────────────────────

QWidget* SystemInfoDialog::buildThreadsTab()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

    m_threadSummary = new QLabel(QStringLiteral("Sampling…"), page);
    // Named so the automation bridge and the tests can address it directly
    // rather than by guessing which QLabel in the dialog this is.
    m_threadSummary->setObjectName(QStringLiteral("systemInfoThreadSummary"));
    layout->addWidget(m_threadSummary);

    m_threadTable = new QTableWidget(0, ColumnCount, page);
    m_threadTable->setHorizontalHeaderLabels(
        {QStringLiteral("Thread"), QStringLiteral("TID"),
         QStringLiteral("State"), QStringLiteral("CPU %"), QStringLiteral("Peak 60 s"),
         QStringLiteral("Total CPU (s)"), QStringLiteral("Last 60 s"), QString()});
    m_threadTable->horizontalHeaderItem(ColState)->setToolTip(
        QStringLiteral("What the thread is doing at the instant of the sample. "
                       "Running means on a core; Waiting means asleep, which is "
                       "what a healthy idle worker looks like.\n\nA dash means "
                       "the platform does not report it. Windows exposes no "
                       "per-thread state, and a value derived from something "
                       "else would not be the same measurement as the other "
                       "two platforms'."));
    m_threadTable->horizontalHeaderItem(ColCpu)->setToolTip(
        QStringLiteral("Share of ONE core, 0-100. Not a share of the machine: a "
                       "single thread pinning one core reads 100 % here however "
                       "many cores are idle."));
    m_threadTable->horizontalHeaderItem(ColPeak)->setToolTip(
        QStringLiteral("Highest CPU % this thread reached in the last 60 s "
                       "(%1 samples at %2 ms). A spike between two glances at "
                       "the CPU % column is invisible without it.\n\nReset "
                       "when the dialog is hidden: sampling stops there, so a "
                       "peak carried across the gap would describe a minute "
                       "nobody observed.")
            .arg(ThreadCpuRing::kSamples)
            .arg(SystemInfoCollector::kSampleIntervalMs));
    m_threadTable->horizontalHeaderItem(ColSpark)->setToolTip(
        QStringLiteral("The same 60 s as Peak, drawn. The vertical scale is "
                       "fixed at 0-100 % of one core on every row, so a tall "
                       "line means a busy thread rather than a thread whose "
                       "own quiet range happened to be scaled up.\n\nSorting "
                       "this column sorts by peak."));
    m_threadTable->setItemDelegateForColumn(ColSpark, new SparklineDelegate(m_threadTable));

    auto* oneDecimal = new OneDecimalDelegate(m_threadTable);
    m_threadTable->setItemDelegateForColumn(ColCpu, oneDecimal);
    m_threadTable->setItemDelegateForColumn(ColPeak, oneDecimal);
    m_threadTable->setItemDelegateForColumn(ColTotal, oneDecimal);
    // Not TID: it is a count, not a measurement, and "243838.0" would be absurd.
    m_threadTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_threadTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_threadTable->setSortingEnabled(true);
    // The name is the variable-width column; the three numeric ones size to
    // their content. Stretching the last section instead left Total CPU
    // enormous and clipped the CPU % header, which is the column the table
    // exists to rank by.
    // The name absorbs slack; the three numeric columns get fixed, readable
    // widths. Sizing them to content instead made them collapse to the width of
    // "0" and pushed Total CPU off the right edge of a wide window.
    m_threadTable->horizontalHeader()->setStretchLastSection(true);   // the spacer
    m_threadTable->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Interactive);
    m_threadTable->horizontalHeader()->setSectionResizeMode(ColTid, QHeaderView::Interactive);
    m_threadTable->horizontalHeader()->setSectionResizeMode(ColState, QHeaderView::Interactive);
    m_threadTable->horizontalHeader()->setSectionResizeMode(ColCpu, QHeaderView::Interactive);
    m_threadTable->horizontalHeader()->setSectionResizeMode(ColPeak, QHeaderView::Interactive);
    m_threadTable->horizontalHeader()->setSectionResizeMode(ColTotal, QHeaderView::Interactive);
    m_threadTable->setColumnWidth(ColName, 280);
    m_threadTable->setColumnWidth(ColTid, 90);
    m_threadTable->setColumnWidth(ColState, 110);
    m_threadTable->setColumnWidth(ColCpu, 80);
    m_threadTable->setColumnWidth(ColPeak, 90);
    m_threadTable->setColumnWidth(ColTotal, 110);
    m_threadTable->setColumnWidth(ColSpark, 130);
    // The hot thread is the question being asked, so it starts at the top.
    m_threadTable->sortItems(ColCpu, Qt::DescendingOrder);
    layout->addWidget(m_threadTable, 1);

    return page;
}

void SystemInfoDialog::applySample(const QVector<ThreadCpuSample>& threads)
{
    if (m_threadTable == nullptr) {
        return;
    }

    // Preserve whatever column the operator sorted by; refilling would
    // otherwise snap the view back and make a moving table unreadable.
    const int sortColumn = m_threadTable->horizontalHeader()->sortIndicatorSection();
    const Qt::SortOrder sortOrder = m_threadTable->horizontalHeader()->sortIndicatorOrder();
    m_threadTable->setSortingEnabled(false);
    m_threadTable->setRowCount(threads.size());

    // Before the rows are filled: Peak reads from the ring, so the newest
    // reading has to be in it first or every peak lags one interval behind the
    // CPU % beside it.
    m_ring.update(threads);

    for (int row = 0; row < threads.size(); ++row) {
        const ThreadCpuSample& sample = threads.at(row);

        auto* nameItem = new QTableWidgetItem(displayName(sample));

        // setData rather than setText for the numeric columns: a text item
        // sorts lexicographically, which puts 9 % above 80 %.
        auto* tidItem = new QTableWidgetItem;
        tidItem->setData(Qt::DisplayRole, static_cast<qulonglong>(sample.tid));
        auto* stateItem = new QTableWidgetItem(stateText(sample.state));
        // Full precision goes in; OneDecimalDelegate does the rounding for
        // display. Rounding here as well would throw away precision the sort
        // can use to separate two threads that differ in the second decimal.
        auto* cpuItem = new QTableWidgetItem;
        cpuItem->setData(Qt::DisplayRole, sample.cpuPercentOfCore);
        auto* peakItem = new QTableWidgetItem;
        peakItem->setData(Qt::DisplayRole, m_ring.peakFor(sample.tid));
        auto* totalItem = new QTableWidgetItem;
        totalItem->setData(Qt::DisplayRole, static_cast<double>(sample.cpuUsecs) / 1e6);

        // The delegate draws the series; the display value exists only so the
        // column has something to sort by, and peak is the reading a sorted
        // sparkline column is being asked about.
        auto* sparkItem = new QTableWidgetItem;
        sparkItem->setData(Qt::DisplayRole, m_ring.peakFor(sample.tid));
        sparkItem->setData(SparklineDelegate::kSeriesRole,
                           QVariant::fromValue(m_ring.seriesFor(sample.tid)));

        tidItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        cpuItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        peakItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        totalItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

        m_threadTable->setItem(row, ColName, nameItem);
        m_threadTable->setItem(row, ColTid, tidItem);
        m_threadTable->setItem(row, ColState, stateItem);
        m_threadTable->setItem(row, ColCpu, cpuItem);
        m_threadTable->setItem(row, ColPeak, peakItem);
        m_threadTable->setItem(row, ColTotal, totalItem);
        m_threadTable->setItem(row, ColSpark, sparkItem);
    }

    m_threadTable->setSortingEnabled(true);
    m_threadTable->sortItems(sortColumn, sortOrder);

    // The shared helper rather than a second inline scan, so the summary line
    // and thresholdExceeded can never disagree about which thread is busiest.
    // It also names an all-idle table's busiest thread instead of falling back
    // to a dash: at 0.0 % that is a reading, not an absence.
    const int busiest = SystemInfo::busiestThreadIndex(threads);
    const double busiestPercent = busiest < 0 ? 0.0 : threads.at(busiest).cpuPercentOfCore;

    // Clearing the alert is this function's job because the crossing signal is
    // edge-triggered: the collector announces going ABOVE the line and says
    // nothing about coming back down.
    if (m_thresholdAlert && busiestPercent <= SystemInfoCollector::kMaxThreadPercentOfCore) {
        m_thresholdAlert = false;
    }

    if (m_threadSummary != nullptr) {
        const QString busiestName = busiest < 0
            ? QStringLiteral("—")
            : displayName(threads.at(busiest));
        m_threadSummary->setText(
            QStringLiteral("%1 threads · busiest %2 at %3 % of one core")
                .arg(threads.size())
                .arg(busiestName)
                .arg(busiestPercent, 0, 'f', 1));
        applyAlertStyle();
    }
}

void SystemInfoDialog::onThresholdExceeded(const QString& threadName, double percentOfCore)
{
    // Acceptance criterion 3 in its minimal form: the summary line goes red.
    // What a louder alert should be — a status-bar badge, a toast — is still an
    // open question on the issue, and the collector's signal is the seam for
    // whichever answer it gets. Nothing is written to the log: this dialog
    // reads the log stream, and having it also produce the events it displays
    // would put its own output into the stream being diagnosed.
    m_thresholdAlert = true;
    m_alertThreadName = threadName.isEmpty() ? QStringLiteral("(unnamed)") : threadName;
    m_alertPercent = percentOfCore;
    applyAlertStyle();
}

void SystemInfoDialog::applyAlertStyle()
{
    if (m_threadSummary == nullptr) {
        return;
    }
    ThemeManager::instance().applyStyleSheet(
        m_threadSummary,
        m_thresholdAlert
            ? QStringLiteral("QLabel { color: {{color.accent.danger}}; font-weight: bold; }")
            : QStringLiteral("QLabel { color: {{color.text.primary}}; }"));
    m_threadSummary->setToolTip(
        m_thresholdAlert
            ? QStringLiteral("%1 crossed %2 %% of one core. One thread saturating "
                             "one core while the others idle is this app's "
                             "characteristic stall, and the status bar's "
                             "system-wide figure cannot show it.")
                  .arg(m_alertThreadName)
                  .arg(SystemInfoCollector::kMaxThreadPercentOfCore, 0, 'f', 0)
            : QString());
}

void SystemInfoDialog::startSampling()
{
    if (m_collectorThread != nullptr) {
        return;
    }
    m_collectorThread = new QThread(this);
    m_collectorThread->setObjectName(QStringLiteral("SystemInfoCollector"));
    m_collector = new SystemInfoCollector;   // no parent — moved to the thread
    m_collector->moveToThread(m_collectorThread);
    // QThread processes deferred deletions after its event loop stops. Keep the
    // worker's destruction on the thread that owns it instead of deleting a
    // foreign-thread QObject from the GUI thread after wait().
    connect(m_collectorThread, &QThread::finished, m_collector, &QObject::deleteLater);
    connect(m_collectorThread, &QThread::started, m_collector, &SystemInfoCollector::init);
    // Queued to this thread; see m_samplingGeneration for why each delivery
    // checks it still belongs to the run that produced it.
    const quint64 generation = ++m_samplingGeneration;
    connect(m_collector, &SystemInfoCollector::sampleReady, this,
            [this, generation](const QVector<ThreadCpuSample>& threads) {
                if (generation == m_samplingGeneration) {
                    applySample(threads);
                }
            });
    connect(m_collector, &SystemInfoCollector::thresholdExceeded, this,
            [this, generation](const QString& threadName, double percentOfCore) {
                if (generation == m_samplingGeneration) {
                    onThresholdExceeded(threadName, percentOfCore);
                }
            });
    connect(m_collector, &SystemInfoCollector::memorySampleReady, this,
            [this, generation](const MemorySample& sample) {
                if (generation == m_samplingGeneration) {
                    applyMemorySample(sample);
                }
            });
    connect(m_collector, &SystemInfoCollector::cpuSampleReady, this,
            [this, generation](const CpuSample& sample) {
                if (generation == m_samplingGeneration) {
                    applyCpuSample(sample);
                }
            });
    // The heartbeat meter has been ticking the whole time this dialog was
    // closed; nobody was reading it. Drop that stretch so the first reading
    // describes the first sampling interval, not the last hour.
    m_tickLagMeter->reset();
    m_collectorThread->start();
}

void SystemInfoDialog::stopSampling()
{
    if (m_collectorThread == nullptr) {
        return;
    }
    // Anything the collector has already queued to us is now stale.
    ++m_samplingGeneration;
    // Tear the timer down on the worker thread FIRST. It was created there, and
    // a QTimer destroyed from another thread is undefined behaviour — Qt reports
    // it as "Timers cannot be stopped from another thread".
    if (m_collector != nullptr && m_collector->thread() != QThread::currentThread()) {
        QMetaObject::invokeMethod(m_collector, "shutdown", Qt::BlockingQueuedConnection);
    }

    m_collectorThread->quit();
    m_collectorThread->wait();
    m_collector = nullptr;
    delete m_collectorThread;
    m_collectorThread = nullptr;

    // Peak claims "the last 60 s". Nothing is sampled while the dialog is
    // hidden, so a ring carried across that gap would describe a minute nobody
    // observed.
    m_ring.clear();
    // The memory ring deliberately stays (and, injected, outlives this dialog):
    // see its declaration. The chart shows the gap instead of pretending the
    // minute was observed.

    // The alert goes with it. A red summary line left standing over a table
    // that stopped updating claims a thread is saturating a core right now,
    // when in fact nothing is being measured at all.
    m_thresholdAlert = false;
    m_alertThreadName.clear();
    m_alertPercent = 0.0;
    applyAlertStyle();
}

// ── Memory ──────────────────────────────────────────────────────────────────

namespace {

// What "resident" means on this platform, in words an operator can read next
// to the number. The snapshot's own names are identifiers, not labels.
QString memoryMetricLabel(const QString& metric)
{
    if (metric == QLatin1String("physicalFootprint")) return QStringLiteral("physical footprint");
    if (metric == QLatin1String("workingSet"))        return QStringLiteral("working set");
    if (metric == QLatin1String("vmRss"))             return QStringLiteral("VmRSS");
    return QStringLiteral("unsupported on this platform");
}

QString megabytesText(quint64 bytes)
{
    return QStringLiteral("%1 MB").arg(MemoryHistoryRing::megabytes(bytes), 0, 'f', 1);
}

}  // namespace

QWidget* SystemInfoDialog::buildMemoryTab()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

    // Header row: what is being measured, and — top-right, where the network
    // dialog keeps its own — how much history the chart shows. The selector
    // lives on this tab rather than the window because Threads has a fixed
    // 60 s window and Logs has none; the network dialog reaches the same
    // outcome by hiding its combo on those pages.
    auto* header = new QHBoxLayout;
    m_memorySummary = new QLabel(QStringLiteral("Sampling…"), page);
    m_memorySummary->setObjectName(QStringLiteral("systemInfoMemorySummary"));
    m_memorySummary->setAccessibleName(QStringLiteral("Process memory summary"));
    header->addWidget(m_memorySummary);
    header->addStretch(1);
    auto* rangeLabel = new QLabel(QStringLiteral("Timeframe"), page);
    rangeLabel->setAccessibleName(QStringLiteral("Chart timeframe"));
    m_memoryRange = new QComboBox(page);
    m_memoryRange->setObjectName(QStringLiteral("systemInfoTimeframe"));
    m_memoryRange->setAccessibleName(QStringLiteral("Chart timeframe"));
    m_memoryRange->setAccessibleDescription(
        QStringLiteral("Choose how much recent memory history the chart displays."));
    m_memoryRange->setFixedWidth(132);
    // The issue's four; the ring holds an hour raw, so nothing longer is offered
    // until the compacting history arrives with the Overview tab.
    m_memoryRange->addItem(QStringLiteral("1 minute"), 60);
    m_memoryRange->addItem(QStringLiteral("5 minutes"), 5 * 60);
    m_memoryRange->addItem(QStringLiteral("15 minutes"), 15 * 60);
    m_memoryRange->addItem(QStringLiteral("1 hour"), 60 * 60);
    m_memoryRange->setCurrentIndex(1);   // 5 minutes: 200 points at 1.5 s
    connect(m_memoryRange, &QComboBox::currentIndexChanged, this,
            &SystemInfoDialog::refreshMemoryChart);
    header->addWidget(rangeLabel);
    header->addWidget(m_memoryRange);
    layout->addLayout(header);

    // Readouts: the numbers, not only the line. Virtual is a readout only —
    // on the chart its scale would flatten the three that move.
    auto* readouts = new QHBoxLayout;
    auto makeReadout = [&](const QString& caption, const QString& objectName,
                           const QString& accessible, const QString& tip) {
        auto* box = new QVBoxLayout;
        auto* cap = new QLabel(caption, page);
        ThemeManager::instance().applyStyleSheet(
            cap, QStringLiteral("QLabel { color: {{color.text.secondary}}; font-size: 10px; }"));
        auto* value = new QLabel(QStringLiteral("—"), page);
        value->setObjectName(objectName);
        value->setAccessibleName(accessible);
        value->setToolTip(tip);
        ThemeManager::instance().applyStyleSheet(
            value, QStringLiteral("QLabel { color: {{color.text.primary}}; font-weight: 700; font-size: 16px; }"));
        box->addWidget(cap);
        box->addWidget(value);
        readouts->addLayout(box);
        return value;
    };
    m_memoryResident = makeReadout(
        QStringLiteral("Resident"), QStringLiteral("systemInfoMemoryResident"),
        QStringLiteral("Resident memory"),
        QStringLiteral("Memory the OS currently holds for this process. What "
                       "\"resident\" measures differs per platform — physical "
                       "footprint on macOS, working set on Windows, VmRSS on "
                       "Linux — and the summary line names which."));
    m_memoryPeak = makeReadout(
        QStringLiteral("Peak"), QStringLiteral("systemInfoMemoryPeak"),
        QStringLiteral("Peak resident memory"),
        QStringLiteral("The highest resident figure the OS has recorded for this "
                       "process since it started. It only goes up, which is why "
                       "the chart plots the current figure beside it. On macOS the "
                       "peak is of the resident size, a different accounting from "
                       "the physical footprint shown as Resident, so it can sit well "
                       "above it."));
    m_memoryPrivate = makeReadout(
        QStringLiteral("Private"), QStringLiteral("systemInfoMemoryPrivate"),
        QStringLiteral("Private memory"),
        QStringLiteral("Memory that belongs to this process alone — not shared "
                       "libraries, not the framebuffer. Growth here that the "
                       "resident figure does not show is a leak's usual shape."));
    m_memoryVirtual = makeReadout(
        QStringLiteral("Virtual"), QStringLiteral("systemInfoMemoryVirtual"),
        QStringLiteral("Virtual address space"),
        QStringLiteral("Address space reserved, not memory used; gigabytes here "
                       "are normal for a GPU-backed Qt process. A readout only: "
                       "on the chart it would flatten the lines that matter."));
    readouts->addStretch(1);
    layout->addLayout(readouts);

    m_memoryGraph = new TimeSeriesGraphWidget(QStringLiteral("Process memory"),
                                              QStringLiteral(" MB"), page);
    m_memoryGraph->setObjectName(QStringLiteral("systemInfoMemoryGraph"));
    m_memoryGraph->setAccessibleName(QStringLiteral("Process memory over time"));
    m_memoryGraph->setToolTip(
        QStringLiteral("Resident, private and peak memory over the selected "
                       "timeframe, sampled every %1 ms while this dialog is "
                       "open. A break in a line is time with no samples: the dialog "
                       "hidden, the machine asleep, or a late tick. "
                       "Click a legend entry to show that series alone.")
            .arg(SystemInfoCollector::kSampleIntervalMs));
    layout->addWidget(m_memoryGraph, 1);
    return page;
}

int SystemInfoDialog::selectedMemoryRangeSeconds() const
{
    if (m_memoryRange == nullptr) {
        return 5 * 60;
    }
    return m_memoryRange->currentData().toInt();
}

void SystemInfoDialog::applyMemorySample(const MemorySample& sample)
{
    // Flat copy into the ring's own record: the ring stays free of core includes.
    MemoryHistoryRing::Record record;
    record.wallMs = sample.wallMs;
    record.valid = sample.valid;
    record.residentMetric = sample.residentMetric;
    record.residentBytes = sample.residentBytes;
    record.peakResidentBytes = sample.peakResidentBytes;
    record.privateBytes = sample.privateBytes;
    record.virtualBytes = sample.virtualBytes;
    m_memoryRing->push(record);
    refreshMemoryChart();
    refreshOverview();   // the Memory card and chart 2 read the same ring
}

void SystemInfoDialog::refreshMemoryChart()
{
    // Readouts come from the ring, not the argument, so a sample that never
    // reached the history cannot look as though it did.
    const MemoryHistoryRing::Record* latest = m_memoryRing->latest();
    if (latest == nullptr) {
        return;
    }
    if (m_memorySummary != nullptr) {
        // An invalid sample names no metric: on Linux residentMetric is set even
        // when the VmRSS read failed (MemoryTelemetry.cpp), and "VmRSS" over four
        // dashes would still read as a measurement.
        m_memorySummary->setText(
            latest->valid
                ? QStringLiteral("Resident = %1 · %2 samples")
                      .arg(memoryMetricLabel(latest->residentMetric))
                      .arg(m_memoryRing->size())
                : QStringLiteral("Process memory: not available on this platform"));
    }
    // A field the platform left at zero is not a measurement — Windows never
    // fills virtualBytes, Linux leaves privateBytes at zero without
    // /proc/self/smaps_rollup, an invalid sample fills nothing — so it reads
    // "—", never "0.0 MB". Each readout then announces its new value the way
    // docs/a11y.md asks for live values (name = caption + value, NameChanged);
    // at 1.5 s this is far below the rate the doc says to throttle.
    const auto show = [&](QLabel* label, const QString& caption, quint64 bytes) {
        if (label == nullptr) {
            return;
        }
        label->setText(latest->valid && bytes > 0 ? megabytesText(bytes)
                                                  : QStringLiteral("\u2014"));
        label->setAccessibleName(QStringLiteral("%1 %2").arg(caption, label->text()));
        QAccessibleEvent event(label, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    };
    show(m_memoryResident, QStringLiteral("Resident memory"), latest->residentBytes);
    show(m_memoryPeak, QStringLiteral("Peak resident memory"), latest->peakResidentBytes);
    show(m_memoryPrivate, QStringLiteral("Private memory"), latest->privateBytes);
    show(m_memoryVirtual, QStringLiteral("Virtual address space"), latest->virtualBytes);

    if (m_memoryGraph == nullptr) {
        return;
    }
    const int rangeSeconds = selectedMemoryRangeSeconds();
    // The window ends at the newest sample, not at the wall clock: a dialog
    // whose sampling is paused shows the history it has, in place, instead of
    // sliding it off the left edge while nothing new arrives.
    const qint64 nowMs = latest->wallMs;
    // Three samples' (or, once bucketed, three buckets') worth: a break drawn
    // where the dialog was hidden, not at every late timer tick, and not
    // between every bucket point of a long range.
    const double gapSeconds = MemoryHistoryRing::connectGapSecondsFor(rangeSeconds);
    using Field = MemoryHistoryRing::Field;
    auto series = [&](const QString& label, const QColor& color, Field field) {
        TimeSeriesGraphWidget::Series s{label, color, {}, QStringLiteral(" MB")};
        s.points = m_memoryRing->series(field, rangeSeconds, nowMs);
        s.maxConnectGapSeconds = gapSeconds;
        return s;
    };
    ThemeManager& theme = ThemeManager::instance();
    m_memoryGraph->setSeries(
        {series(QStringLiteral("Resident"), theme.color("color.accent"), Field::Resident),
         series(QStringLiteral("Private"),  theme.color("color.accent.success"), Field::Private),
         series(QStringLiteral("Peak"),     theme.color("color.accent.warning"), Field::Peak)},
        rangeSeconds);
}

// ── Overview ────────────────────────────────────────────────────────────────

QWidget* SystemInfoDialog::buildOverviewTab()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

    // Timeframe top-right, as on the Memory tab and in the network dialog;
    // per tab rather than per window for the reason the Memory tab gives.
    auto* header = new QHBoxLayout;
    header->addStretch(1);
    auto* rangeLabel = new QLabel(QStringLiteral("Timeframe"), page);
    rangeLabel->setAccessibleName(QStringLiteral("Chart timeframe"));
    m_overviewRange = new QComboBox(page);
    m_overviewRange->setObjectName(QStringLiteral("systemInfoOverviewTimeframe"));
    m_overviewRange->setAccessibleName(QStringLiteral("Chart timeframe"));
    m_overviewRange->setAccessibleDescription(
        QStringLiteral("Choose how much recent history the Overview charts display."));
    m_overviewRange->setFixedWidth(132);
    m_overviewRange->addItem(QStringLiteral("1 minute"), 60);
    m_overviewRange->addItem(QStringLiteral("5 minutes"), 5 * 60);
    m_overviewRange->addItem(QStringLiteral("15 minutes"), 15 * 60);
    m_overviewRange->addItem(QStringLiteral("1 hour"), 60 * 60);
    m_overviewRange->setCurrentIndex(1);
    connect(m_overviewRange, &QComboBox::currentIndexChanged, this,
            &SystemInfoDialog::refreshOverview);
    header->addWidget(rangeLabel);
    header->addWidget(m_overviewRange);
    layout->addLayout(header);

    // Four cards across, the network dialog's row.
    auto* cards = new QHBoxLayout;
    const QString startingValues = QStringLiteral(
        " These bands are the issue's starting values, not measured ones.");
    OverviewCard cpu = makeOverviewCard(
        QStringLiteral("CPU Total"), QStringLiteral("% of system capacity"),
        QStringLiteral("systemInfoCardCpu"), QStringLiteral("CPU total"),
        QStringLiteral("This process's share of the whole machine — every "
                       "thread's core-time summed and divided by the core "
                       "count, the same figure as the status bar's CPU label. "
                       "Yellow at %1 %, red at %2 %.")
                .arg(kCpuWarnPercent, 0, 'f', 0).arg(kCpuDangerPercent, 0, 'f', 0)
            + startingValues,
        page);
    m_cardCpuValue = cpu.value;
    OverviewCard maxThread = makeOverviewCard(
        QStringLiteral("Max Thread"), QStringLiteral("% of one core"),
        QStringLiteral("systemInfoCardMaxThread"), QStringLiteral("Busiest thread"),
        QStringLiteral("The busiest single thread's share of ONE core in the "
                       "last sample — the figure the status bar's system-wide "
                       "percentage hides (#2545). Yellow at %1 %, red at %2 %.")
                .arg(kMaxThreadWarnPercent, 0, 'f', 0).arg(kMaxThreadDangerPercent, 0, 'f', 0)
            + startingValues,
        page);
    m_cardMaxThreadValue = maxThread.value;
    m_cardMaxThreadCaption = maxThread.caption;
    OverviewCard memory = makeOverviewCard(
        QStringLiteral("Memory"), QStringLiteral("Resident set"),
        QStringLiteral("systemInfoCardMemory"), QStringLiteral("Resident memory"),
        QStringLiteral("Memory the OS currently holds for this process; the "
                       "Memory tab names what \"resident\" measures on this "
                       "platform. Yellow at %1 GB, red at %2 GB.")
                .arg(kMemoryWarnMb / 1024.0, 0, 'f', 0).arg(kMemoryDangerMb / 1024.0, 0, 'f', 0)
            + startingValues,
        page);
    m_cardMemoryValue = memory.value;
    OverviewCard tickLag = makeOverviewCard(
        QStringLiteral("GUI Tick Lag"), QStringLiteral("Worst event-loop delay, last sample"),
        QStringLiteral("systemInfoCardTickLag"), QStringLiteral("GUI tick lag"),
        QStringLiteral("How late the GUI thread's 50 ms heartbeat timer fired, "
                       "at worst, during the last sampling interval: the "
                       "event loop was busy for that long. Zero is on time. "
                       "Not a frame rate, and not banded — no measured "
                       "baseline exists yet for what \"late\" should mean here."),
        page);
    m_cardTickLagValue = tickLag.value;
    for (QFrame* frame : {cpu.frame, maxThread.frame, memory.frame, tickLag.frame}) {
        cards->addWidget(frame, 1);
    }
    layout->addLayout(cards);

    // Charts, 2 × 2, the issue's four.
    auto* grid = new QGridLayout;
    m_overviewCpuGraph = new TimeSeriesGraphWidget(QStringLiteral("CPU"), QStringLiteral("%"), page);
    m_overviewCpuGraph->setObjectName(QStringLiteral("systemInfoOverviewCpuGraph"));
    m_overviewCpuGraph->setAccessibleName(QStringLiteral("CPU over time"));
    m_overviewCpuGraph->setToolTip(
        QStringLiteral("Process: share of the whole machine. Busiest thread: that "
                       "thread's share of one core. Both are percentages, on one "
                       "0–100 axis. Click a legend entry to show that series alone."));
    m_overviewCpuGraph->setFixedYRange(0.0, 100.0);
    m_overviewMemoryGraph = new TimeSeriesGraphWidget(QStringLiteral("Memory"), QStringLiteral(" MB"), page);
    m_overviewMemoryGraph->setObjectName(QStringLiteral("systemInfoOverviewMemoryGraph"));
    m_overviewMemoryGraph->setAccessibleName(QStringLiteral("Memory over time"));
    m_overviewMemoryGraph->setToolTip(
        QStringLiteral("Resident memory with the OS's recorded peak beside it; "
                       "the Memory tab has the full set of readouts."));
    m_overviewThreadsGraph = new TimeSeriesGraphWidget(QStringLiteral("Top threads"), QStringLiteral("%"), page);
    m_overviewThreadsGraph->setObjectName(QStringLiteral("systemInfoOverviewThreadsGraph"));
    m_overviewThreadsGraph->setAccessibleName(QStringLiteral("Top threads"));
    m_overviewThreadsGraph->setToolTip(
        QStringLiteral("The %1 threads with the highest mean share of a core over "
                       "the selected timeframe, each as its own line in percent of one "
                       "core, so one hot thread stands clear of the rest. Click a "
                       "legend entry to show that thread alone.")
            .arg(CpuHistoryRing::kTopThreads));
    m_overviewTickGraph = new TimeSeriesGraphWidget(QStringLiteral("GUI tick lag"), QStringLiteral(" ms"), page);
    m_overviewTickGraph->setObjectName(QStringLiteral("systemInfoOverviewTickGraph"));
    m_overviewTickGraph->setAccessibleName(QStringLiteral("GUI tick lag over time"));
    m_overviewTickGraph->setToolTip(
        QStringLiteral("How late the GUI thread's 50 ms heartbeat fired — the "
                       "worst and the mean lag per sampling interval. A busy "
                       "event loop shows here before it shows as a stalled "
                       "window."));
    grid->addWidget(m_overviewCpuGraph, 0, 0);
    grid->addWidget(m_overviewMemoryGraph, 0, 1);
    grid->addWidget(m_overviewThreadsGraph, 1, 0);
    grid->addWidget(m_overviewTickGraph, 1, 1);
    grid->setRowStretch(0, 1);
    grid->setRowStretch(1, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid, 1);
    return page;
}

int SystemInfoDialog::selectedOverviewRangeSeconds() const
{
    if (m_overviewRange == nullptr) {
        return 5 * 60;
    }
    return m_overviewRange->currentData().toInt();
}

void SystemInfoDialog::setCardLevel(QLabel* value, SystemInfo::CardLevel level)
{
    if (value == nullptr) {
        return;
    }
    const char* token = "{{color.text.primary}}";
    const char* name = "normal";
    if (level == SystemInfo::CardLevel::Danger) {
        token = "{{color.accent.danger}}";
        name = "danger";
    } else if (level == SystemInfo::CardLevel::Warning) {
        token = "{{color.accent.warning}}";
        name = "warning";
    }
    ThemeManager::instance().applyStyleSheet(
        value, QStringLiteral("QLabel { color: %1; font-weight: 700; font-size: 18px; "
                              "background: transparent; }")
                   .arg(QLatin1String(token)));
    value->setProperty("level", QLatin1String(name));
}

void SystemInfoDialog::applyCpuSample(const CpuSample& sample)
{
    CpuHistoryRing::Record record;
    record.wallMs = sample.wallMs;
    record.valid = true;
    record.coreCount = sample.coreCount;
    record.processPercentOfCapacity = sample.processPercentOfCapacity;
    record.busiestTid = sample.busiestTid;
    record.busiestName = sample.busiestName;
    record.busiestPercentOfCore = sample.busiestPercentOfCore;
    record.threads.reserve(sample.busyThreads.size());
    for (const ThreadCpuSample& t : sample.busyThreads) {
        CpuHistoryRing::ThreadShare share;
        share.tid = t.tid;
        share.name = t.name;
        share.percentOfCore = t.cpuPercentOfCore;
        record.threads.push_back(share);
    }
    // The heartbeat meter is read here, on the GUI thread, at the moment the
    // CPU reading lands, so the tick-lag figure describes the same interval.
    const UiTickLagMeter::Reading lag = m_tickLagMeter->take();
    record.tickCount = lag.tickCount;
    record.tickLagMaxMs = lag.lagMaxMs;
    record.tickLagMeanMs = lag.lagMeanMs;
    m_cpuRing->push(record);
    refreshOverview();
}

void SystemInfoDialog::refreshOverview()
{
    const CpuHistoryRing::Record* cpu = m_cpuRing->latest();
    const MemoryHistoryRing::Record* mem = m_memoryRing->latest();
    const int rangeSeconds = selectedOverviewRangeSeconds();
    const double gapSeconds = CpuHistoryRing::connectGapSecondsFor(rangeSeconds);
    ThemeManager& theme = ThemeManager::instance();

    // Cards. Each announces its value as the Memory readouts do (docs/a11y.md).
    const auto announce = [](QLabel* label, const QString& caption) {
        if (label == nullptr) {
            return;
        }
        label->setAccessibleName(QStringLiteral("%1 %2").arg(caption, label->text()));
        QAccessibleEvent event(label, QAccessible::NameChanged);
        QAccessible::updateAccessibility(&event);
    };
    const QString dash = QStringLiteral("\u2014");
    if (m_cardCpuValue != nullptr) {
        const bool have = cpu != nullptr && cpu->valid;
        m_cardCpuValue->setText(have ? QStringLiteral("%1 %").arg(cpu->processPercentOfCapacity, 0, 'f', 1) : dash);
        setCardLevel(m_cardCpuValue, have ? SystemInfo::cardLevel(cpu->processPercentOfCapacity,
                                                                    kCpuWarnPercent, kCpuDangerPercent)
                                          : SystemInfo::CardLevel::Normal);
        announce(m_cardCpuValue, QStringLiteral("CPU total"));
    }
    if (m_cardMaxThreadValue != nullptr) {
        const bool have = cpu != nullptr && cpu->valid;
        m_cardMaxThreadValue->setText(have ? QStringLiteral("%1 %").arg(cpu->busiestPercentOfCore, 0, 'f', 1) : dash);
        setCardLevel(m_cardMaxThreadValue, have ? SystemInfo::cardLevel(cpu->busiestPercentOfCore,
                                                                          kMaxThreadWarnPercent, kMaxThreadDangerPercent)
                                                : SystemInfo::CardLevel::Normal);
        if (m_cardMaxThreadCaption != nullptr) {
            m_cardMaxThreadCaption->setText(
                have ? QStringLiteral("%1 · % of one core")
                           .arg(cpu->busiestName.isEmpty() ? QStringLiteral("(unnamed)") : cpu->busiestName)
                     : QStringLiteral("% of one core"));
        }
        announce(m_cardMaxThreadValue, QStringLiteral("Busiest thread"));
    }
    if (m_cardMemoryValue != nullptr) {
        const bool have = mem != nullptr && mem->valid && mem->residentBytes > 0;
        m_cardMemoryValue->setText(have ? megabytesText(mem->residentBytes) : dash);
        setCardLevel(m_cardMemoryValue, have ? SystemInfo::cardLevel(MemoryHistoryRing::megabytes(mem->residentBytes),
                                                                       kMemoryWarnMb, kMemoryDangerMb)
                                             : SystemInfo::CardLevel::Normal);
        announce(m_cardMemoryValue, QStringLiteral("Resident memory"));
    }
    if (m_cardTickLagValue != nullptr) {
        const bool have = cpu != nullptr && cpu->valid && cpu->tickCount > 0;
        m_cardTickLagValue->setText(have ? QStringLiteral("%1 ms").arg(cpu->tickLagMaxMs, 0, 'f', 1) : dash);
        setCardLevel(m_cardTickLagValue, SystemInfo::CardLevel::Normal);   // unbanded, see kCpuWarnPercent
        announce(m_cardTickLagValue, QStringLiteral("GUI tick lag"));
    }

    // Charts. Each window ends at its own ring's newest sample (see
    // refreshMemoryChart for why not the wall clock).
    using CpuField = CpuHistoryRing::Field;
    using MemField = MemoryHistoryRing::Field;
    const auto cpuSeries = [&](const QString& label, const QColor& color, CpuField field, const QString& suffix) {
        TimeSeriesGraphWidget::Series s{label, color, {}, suffix};
        if (cpu != nullptr) {
            s.points = m_cpuRing->series(field, rangeSeconds, cpu->wallMs);
        }
        s.maxConnectGapSeconds = gapSeconds;
        return s;
    };
    if (m_overviewCpuGraph != nullptr) {
        m_overviewCpuGraph->setSeries(
            {cpuSeries(QStringLiteral("Process"), theme.color("color.accent"), CpuField::ProcessPercent, QStringLiteral("%")),
             cpuSeries(QStringLiteral("Busiest thread"), theme.color("color.accent.warning"), CpuField::BusiestPercent, QStringLiteral("%"))},
            rangeSeconds);
    }
    if (m_overviewTickGraph != nullptr) {
        m_overviewTickGraph->setSeries(
            {cpuSeries(QStringLiteral("Worst"), theme.color("color.accent.danger"), CpuField::TickLagMax, QStringLiteral(" ms")),
             cpuSeries(QStringLiteral("Mean"), theme.color("color.accent"), CpuField::TickLagMean, QStringLiteral(" ms"))},
            rangeSeconds);
    }
    if (m_overviewMemoryGraph != nullptr) {
        const auto memSeries = [&](const QString& label, const QColor& color, MemField field) {
            TimeSeriesGraphWidget::Series s{label, color, {}, QStringLiteral(" MB")};
            if (mem != nullptr) {
                s.points = m_memoryRing->series(field, rangeSeconds, mem->wallMs);
            }
            s.maxConnectGapSeconds = MemoryHistoryRing::connectGapSecondsFor(rangeSeconds);
            return s;
        };
        m_overviewMemoryGraph->setSeries(
            {memSeries(QStringLiteral("Resident"), theme.color("color.accent"), MemField::Resident),
             memSeries(QStringLiteral("Peak"), theme.color("color.accent.warning"), MemField::Peak)},
            rangeSeconds);
    }
    if (m_overviewThreadsGraph != nullptr) {
        QVector<TimeSeriesGraphWidget::Series> lines;
        if (cpu != nullptr) {
            const QVector<CpuHistoryRing::ThreadSeries> top =
                m_cpuRing->topThreadSeries(CpuHistoryRing::kTopThreads, rangeSeconds, cpu->wallMs);
            // Five distinct accent tokens; the palette is the theme's, not ours.
            static const char* const kThreadTokens[] = {
                "color.accent", "color.accent.success", "color.accent.warning",
                "color.accent.bright", "color.accent.danger"};
            QHash<QString, int> nameCount;
            for (const CpuHistoryRing::ThreadSeries& ts : top) {
                ++nameCount[ts.name];
            }
            for (int i = 0; i < top.size(); ++i) {
                const CpuHistoryRing::ThreadSeries& ts = top.at(i);
                TimeSeriesGraphWidget::Series s{threadLabel(ts, nameCount.value(ts.name) > 1),
                                                theme.color(kThreadTokens[i % 5]), ts.points,
                                                QStringLiteral("%")};
                s.maxConnectGapSeconds = gapSeconds;
                lines.push_back(s);
            }
        }
        m_overviewThreadsGraph->setSeries(lines, rangeSeconds);
    }
}

// ── Logs ────────────────────────────────────────────────────────────────────

QWidget* SystemInfoDialog::buildLogsTab()
{
    auto* page = new QWidget;
    m_logsPage = page;
    auto* layout = new QVBoxLayout(page);

    // Three checkboxes fit a row with room to spare. The scroll area this used
    // to need, and the legend that had scrolled out of reach inside it, went
    // with the twenty-five categories that are no longer offered here.
    m_filterRow = new QHBoxLayout;
    layout->addLayout(m_filterRow);

    // Follow the categories that are actually switched on, live. A fixed list
    // would go stale as categories are added, and — worse — would offer a
    // ticked box above an empty pane whenever the category behind it was not
    // being logged at all, which says "showing" while meaning "nothing is
    // being written".
    connect(&LogManager::instance(), &LogManager::categoryChanged,
            this, [this](const QString&, bool) {
                rebuildCategoryFilters();
                rebuildLogView();
            });
    auto* infoRow = new QHBoxLayout;
    // Which file this is. Without it an empty pane is ambiguous between "no
    // matching lines" and "following something other than what you think".
    m_logPathLabel = new QLabel(page);
    m_logPathLabel->setObjectName(QStringLiteral("systemInfoLogPath"));
    m_logPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ThemeManager::instance().applyStyleSheet(
        m_logPathLabel,
        QStringLiteral("QLabel { color: {{color.text.secondary}}; font-size: 11px; }"));
    infoRow->addWidget(m_logPathLabel, 1);
    m_logLiveToggle = new QPushButton(QStringLiteral("Live"), page);
    m_logLiveToggle->setObjectName(QStringLiteral("systemInfoLogLiveToggle"));
    m_logLiveToggle->setCheckable(true);
    m_logLiveToggle->setChecked(true);
    m_logLiveToggle->setFixedWidth(92);
    connect(m_logLiveToggle, &QPushButton::toggled,
            this, [this](bool live) { setLogFollowLive(live); });
    infoRow->addWidget(m_logLiveToggle);
    layout->addLayout(infoRow);

    m_logViewer = new QPlainTextEdit(page);
    m_logViewer->setObjectName(QStringLiteral("systemInfoLogViewer"));
    m_logViewer->setReadOnly(true);
    m_logViewer->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_logViewer->setMaximumBlockCount(static_cast<int>(kMaxStoredLines));
    new LogSyntaxHighlighter(m_logViewer->document());
    layout->addWidget(m_logViewer, 1);

    // Scrolling up IS the pause gesture. Reading a stall means holding still on
    // the lines around it, and a view that yanks itself back to the bottom
    // every 500 ms cannot be read at all — the operator would have to find the
    // button before the log became legible.
    connect(m_logViewer->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this](int value) {
                if (m_handlingLogScroll || m_logViewer == nullptr) {
                    return;
                }
                if (value < m_logViewer->verticalScrollBar()->maximum()) {
                    setLogFollowLive(false);
                }
            });

    setLogFollowLive(true);   // establishes the button's text and tooltip

    // No checkbox, never removed — see kNoticeCategory.
    m_enabledCategories.insert(QString::fromLatin1(kNoticeCategory));

    rebuildCategoryFilters();
    return page;
}

void SystemInfoDialog::rebuildCategoryFilters()
{
    if (m_filterRow == nullptr || m_logsPage == nullptr) {
        return;   // called before the page exists; nothing to parent widgets to
    }

    // Snapshot which categories we already had boxes for BEFORE clearing them:
    // it is the only way to tell "new box, start it on" from "the operator
    // unticked this one, leave it unticked".
    QSet<QString> previouslyKnown;
    for (auto it = m_categoryBoxes.constBegin(); it != m_categoryBoxes.constEnd(); ++it) {
        previouslyKnown.insert(it.key());
    }

    while (QLayoutItem* item = m_filterRow->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_categoryBoxes.clear();

    m_filterRow->addWidget(new QLabel(QStringLiteral("Show:"), m_logsPage));

    QHash<QString, LogManager::Category> byId;
    for (const LogManager::Category& category : LogManager::instance().categories()) {
        byId.insert(category.id, category);
    }

    bool anyWarningsOnly = false;
    for (const char* const id : kPerfCategories) {
        const QString key = QString::fromLatin1(id);
        const auto found = byId.constFind(key);
        if (found == byId.constEnd()) {
            continue;   // registry changed under us; better a missing box than a crash
        }

        auto* box = new QCheckBox(found->label, m_logsPage);
        box->setObjectName(QStringLiteral("logFilter_%1").arg(key));

        // A category switched OFF in LogManager still writes warnings and
        // criticals — "Default state: all debug logging DISABLED.
        // Warnings/criticals always pass" (LogManager.h:58). Skipping it, as
        // this row used to, made those lines undisplayable: the most important
        // lines in the file, hidden by the filter meant to reveal them. It gets
        // a box, dimmed, saying what it will and will not show — which also
        // answers the ticked-box-over-an-empty-pane problem honestly rather
        // than by hiding the box.
        if (found->enabled) {
            box->setToolTip(QStringLiteral("%1 — %2").arg(key, found->description));
        } else {
            anyWarningsOnly = true;
            ThemeManager::instance().applyStyleSheet(
                box, QStringLiteral("QCheckBox { color: {{color.text.disabled}}; }"));
            box->setToolTip(
                QStringLiteral("%1 — %2\n\nWarnings and criticals only: this "
                               "category's full logging is switched off. Turn it "
                               "on in Help \u2192 Support & Diagnostics.")
                    .arg(key, found->description));
        }

        box->setChecked(previouslyKnown.contains(key)
                            ? m_enabledCategories.contains(key)
                            : true);
        if (box->isChecked()) {
            m_enabledCategories.insert(key);
        } else {
            m_enabledCategories.remove(key);
        }
        connect(box, &QCheckBox::toggled, this, [this, key](bool on) {
            if (on) {
                m_enabledCategories.insert(key);
            } else {
                m_enabledCategories.remove(key);
            }
            rebuildLogView();
        });
        m_categoryBoxes.insert(key, box);
        m_filterRow->addWidget(box);
    }

    m_filterRow->addStretch(1);

    // Only when it has something to explain. A permanent legend beside three
    // bright boxes is noise.
    if (anyWarningsOnly) {
        auto* legend = new QLabel(QStringLiteral("dimmed = warnings only"), m_logsPage);
        legend->setObjectName(QStringLiteral("systemInfoFilterLegend"));
        ThemeManager::instance().applyStyleSheet(
            legend, QStringLiteral("QLabel { color: {{color.text.disabled}}; }"));
        legend->setToolTip(
            QStringLiteral("A dimmed category is switched off in Help \u2192 "
                           "Support & Diagnostics, so only its warnings and criticals reach the "
                           "log at all. Ticking it here shows those."));
        m_filterRow->addWidget(legend);
    }
}

QString SystemInfoDialog::categoryFromLine(const QString& line)
{
    // "[time] LEVEL category: message" — the same shape the network dialog
    // parses, kept identical so the two tails agree about what a category is.
    static const QRegularExpression categoryRe(
        QStringLiteral("^\\[[^\\]]+\\]\\s+\\S+\\s+([^:]+):"));
    const QRegularExpressionMatch match = categoryRe.match(line);
    if (!match.hasMatch()) {
        return QStringLiteral("default");
    }
    const QString category = match.captured(1).trimmed();
    return category.isEmpty() ? QStringLiteral("default") : category;
}

void SystemInfoDialog::openLogTail()
{
    // The poll runs whether or not the first open succeeded: logFilePath()
    // always names a file, and one that does not exist yet — logging switched
    // on after the dialog opened, or a rotation in flight — is retried by
    // pollLog() rather than left dead until the dialog is hidden and shown.
    // A temporary hide leaves an open handle at its current offset, so showing
    // the dialog again catches up without replaying the initial 64 KiB tail.
    if (!m_logFile.isOpen()) {
        reopenLogTail(LogManager::instance().logFilePath());
    }
    if (m_logFile.isOpen()) {
        pollLog();
    }

    if (m_logTimer == nullptr) {
        m_logTimer = new QTimer(this);
        m_logTimer->setInterval(kLogPollMs);
        connect(m_logTimer, &QTimer::timeout, this, &SystemInfoDialog::pollLog);
    }
    m_logTimer->start();
}

bool SystemInfoDialog::reopenLogTail(const QString& path)
{
    if (m_logPathLabel != nullptr) {
        m_logPathLabel->setText(path.isEmpty()
                                    ? QStringLiteral("Log: (not writing to a file)")
                                    : QStringLiteral("Log: %1").arg(path));
    }
    if (path.isEmpty()) {
        return false;
    }
    m_logFile.close();
    m_logPartialLine.clear();
    m_logFile.setFileName(path);
    if (!m_logFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    // Start from the tail, not the beginning: a long session's log is tens of
    // megabytes and none of it is the stall being investigated right now. A
    // file shorter than the window is read whole, which is also what a
    // just-rotated log wants.
    const qint64 size = m_logFile.size();
    if (size > kInitialTailBytes) {
        m_logFile.seek(size - kInitialTailBytes);
        m_logFile.readLine();  // discard the partial line the seek landed in
    }
    return true;
}

void SystemInfoDialog::pauseLogTail()
{
    if (m_logTimer != nullptr) {
        m_logTimer->stop();
    }
}

void SystemInfoDialog::pollLog()
{
    if (!m_logFile.isOpen()) {
        // Not open yet — see openLogTail(). Keep trying at the poll cadence.
        reopenLogTail(LogManager::instance().logFilePath());
        if (!m_logFile.isOpen()) {
            return;
        }
    }

    // The file can move out from under a tail that has been running for hours:
    // rotated, restarted by LogManager, or replaced at the same path. Reading
    // from a stale handle looks exactly like a log that simply stopped — the
    // pane goes quiet and nothing says why, which during an investigation reads
    // as "the app stopped logging" rather than "this view is stuck".
    const QString currentPath = LogManager::instance().logFilePath();
    const QFileInfo info(currentPath);
    const bool pathChanged = m_logFile.fileName() != currentPath;
    const bool truncated = info.exists() && info.size() < m_logFile.pos();
    if (pathChanged || truncated) {
        if (!reopenLogTail(currentPath)) {
            return;
        }
        appendLogLine(QStringLiteral("[--:--:--.---] INF %1: Log file was %2; "
                                     "following the current one from here")
                          .arg(QLatin1String(kNoticeCategory),
                               pathChanged ? QStringLiteral("replaced")
                                           : QStringLiteral("reset")));
    }

    // Whole lines only. Whatever follows the last newline stays in
    // m_logPartialLine until the writer completes it; the network dialog's
    // tail does the same, and without it a line caught mid-write would be
    // shown truncated and its tail dropped as an uncategorised fragment.
    m_logPartialLine += m_logFile.readAll();
    int newline = -1;
    while ((newline = m_logPartialLine.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_logPartialLine.left(newline)).trimmed();
        m_logPartialLine.remove(0, newline + 1);
        if (!line.isEmpty()) {
            appendLogLine(line);
        }
    }
}

void SystemInfoDialog::appendLogLine(const QString& line)
{
    const QString category = categoryFromLine(line);
    m_logLines.push_back({category, line});
    while (m_logLines.size() > kMaxStoredLines) {
        m_logLines.removeFirst();
    }
    // Retained either way: pausing must not lose the lines that arrive while
    // the operator is reading, or turning Live back on would show a gap.
    if (m_logViewer == nullptr || !m_logFollowLive
        || !m_enabledCategories.contains(category)) {
        return;
    }
    m_logViewer->appendPlainText(line);
    m_handlingLogScroll = true;
    m_logViewer->verticalScrollBar()->setValue(
        m_logViewer->verticalScrollBar()->maximum());
    m_handlingLogScroll = false;
    // appendPlainText leaves the cursor at the end of the line, which scrolls a
    // no-wrap viewport right and hides the timestamp and category — the two
    // fields you read first. Pin the view back to the left margin.
    m_logViewer->horizontalScrollBar()->setValue(0);
}

void SystemInfoDialog::rebuildLogView()
{
    if (m_logViewer == nullptr) {
        return;
    }
    // Re-filtering replays what was kept rather than re-reading the file, so
    // toggling a category cannot lose lines that have already rolled past.
    m_handlingLogScroll = true;
    m_logViewer->clear();
    for (const auto& entry : m_logLines) {
        if (m_enabledCategories.contains(entry.first)) {
            m_logViewer->appendPlainText(entry.second);
        }
    }
    // Only jump to the newest line if we are following it. Ticking a category
    // while paused would otherwise throw the operator back to the bottom,
    // which is precisely what pausing was for.
    if (m_logFollowLive) {
        m_logViewer->verticalScrollBar()->setValue(
            m_logViewer->verticalScrollBar()->maximum());
    }
    m_logViewer->horizontalScrollBar()->setValue(0);
    m_handlingLogScroll = false;
}

void SystemInfoDialog::setLogFollowLive(bool on)
{
    m_logFollowLive = on;
    if (m_logLiveToggle != nullptr) {
        // Blocked: this is also called BY the button, and re-entering its
        // toggled signal would fight the scrollbar handler.
        const QSignalBlocker blocker(m_logLiveToggle);
        m_logLiveToggle->setChecked(on);
        m_logLiveToggle->setText(on ? QStringLiteral("Live") : QStringLiteral("Paused"));
        m_logLiveToggle->setToolTip(
            on ? QStringLiteral("Following the newest output. Scroll up, or turn "
                                "this off, to hold still on older lines.")
               : QStringLiteral("Paused. Lines are still being collected — turn "
                                "Live back on to catch up to the newest."));
    }
    // Catching up replays everything retained while paused, so resuming shows
    // the lines that arrived rather than resuming from wherever the file is now.
    if (on) {
        rebuildLogView();
    }
}

// ── Visibility ──────────────────────────────────────────────────────────────

void SystemInfoDialog::showEvent(QShowEvent* event)
{
    PersistentDialog::showEvent(event);
    startSampling();
    openLogTail();
}

void SystemInfoDialog::hideEvent(QHideEvent* event)
{
    stopSampling();
    pauseLogTail();
    PersistentDialog::hideEvent(event);
}

}  // namespace AetherSDR
