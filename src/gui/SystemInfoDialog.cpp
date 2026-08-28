#include "SystemInfoDialog.h"

#include "LogSyntaxHighlighter.h"
#include "SparklineDelegate.h"
#include "core/LogManager.h"
#include "core/SystemInfoCollector.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QFrame>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

constexpr int kLogPollMs = 500;
constexpr qint64 kInitialTailBytes = 64 * 1024;
constexpr qsizetype kMaxStoredLines = 5000;

// A trailing spacer column soaks up slack on a wide window. Without it the
// stretch has to land on a real column, and the thread name — the only one
// that varies — ends up several hundred pixels wider than the longest name.
// An unnamed row is a raw std::thread Qt never saw — say so rather than leaving
// a blank cell that reads as a rendering fault. Shared so the table cell and the
// summary line cannot describe the same thread differently.
QString displayName(const ThreadCpuSample& sample)
{
    return sample.name.isEmpty() ? QStringLiteral("(unnamed)") : sample.name;
}

// Column order follows the issue's own list for the Threads tab.
enum Column { ColName = 0, ColTid, ColCpu, ColPeak, ColTotal, ColSpark, ColSpacer,
              ColumnCount };

}  // namespace

SystemInfoDialog::SystemInfoDialog(QWidget* parent)
    : PersistentDialog(QStringLiteral("System Info"),
                       QStringLiteral("SystemInfoDialogGeometry"), parent)
{
    auto* layout = new QVBoxLayout(bodyWidget());
    auto* tabs = new QTabWidget(bodyWidget());
    tabs->addTab(buildThreadsTab(), QStringLiteral("Threads"));
    tabs->addTab(buildLogsTab(), QStringLiteral("Logs"));
    layout->addWidget(tabs);
    resize(900, 600);
}

SystemInfoDialog::~SystemInfoDialog()
{
    stopSampling();
    closeLogTail();
}

// ── Threads ─────────────────────────────────────────────────────────────────

QWidget* SystemInfoDialog::buildThreadsTab()
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

    m_threadSummary = new QLabel(QStringLiteral("Sampling…"), page);
    layout->addWidget(m_threadSummary);

    m_threadTable = new QTableWidget(0, ColumnCount, page);
    m_threadTable->setHorizontalHeaderLabels(
        {QStringLiteral("Thread"), QStringLiteral("TID"),
         QStringLiteral("CPU %"), QStringLiteral("Peak 60 s"),
         QStringLiteral("Total CPU (s)"), QStringLiteral("Last 60 s"), QString()});
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
    m_threadTable->horizontalHeader()->setSectionResizeMode(ColCpu, QHeaderView::Interactive);
    m_threadTable->horizontalHeader()->setSectionResizeMode(ColPeak, QHeaderView::Interactive);
    m_threadTable->horizontalHeader()->setSectionResizeMode(ColTotal, QHeaderView::Interactive);
    m_threadTable->setColumnWidth(ColName, 280);
    m_threadTable->setColumnWidth(ColTid, 90);
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
        auto* cpuItem = new QTableWidgetItem;
        cpuItem->setData(Qt::DisplayRole, QString::number(sample.cpuPercentOfCore, 'f', 1).toDouble());
        auto* peakItem = new QTableWidgetItem;
        peakItem->setData(Qt::DisplayRole,
                          QString::number(m_ring.peakFor(sample.tid), 'f', 1).toDouble());
        auto* totalItem = new QTableWidgetItem;
        totalItem->setData(Qt::DisplayRole,
                           QString::number(static_cast<double>(sample.cpuUsecs) / 1e6, 'f', 1).toDouble());

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
        m_threadTable->setItem(row, ColCpu, cpuItem);
        m_threadTable->setItem(row, ColPeak, peakItem);
        m_threadTable->setItem(row, ColTotal, totalItem);
        m_threadTable->setItem(row, ColSpark, sparkItem);
    }

    m_threadTable->setSortingEnabled(true);
    m_threadTable->sortItems(sortColumn, sortOrder);

    if (m_threadSummary != nullptr) {
        // The shared helper rather than a second inline scan, so the summary
        // line and thresholdExceeded can never disagree about which thread is
        // busiest. It also names an all-idle table's busiest thread instead of
        // falling back to a dash: at 0.0 % that is a reading, not an absence.
        const int busiest = SystemInfo::busiestThreadIndex(threads);
        const QString busiestName = busiest < 0
            ? QStringLiteral("—")
            : displayName(threads.at(busiest));
        m_threadSummary->setText(
            QStringLiteral("%1 threads · busiest %2 at %3 % of one core")
                .arg(threads.size())
                .arg(busiestName)
                .arg(busiest < 0 ? 0.0 : threads.at(busiest).cpuPercentOfCore, 0, 'f', 1));
    }
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
    connect(m_collectorThread, &QThread::started, m_collector, &SystemInfoCollector::init);
    connect(m_collector, &SystemInfoCollector::sampleReady,
            this, &SystemInfoDialog::applySample);
    m_collectorThread->start();
}

void SystemInfoDialog::stopSampling()
{
    if (m_collectorThread == nullptr) {
        return;
    }
    // Tear the timer down on the worker thread FIRST. It was created there, and
    // a QTimer destroyed from another thread is undefined behaviour — Qt reports
    // it as "Timers cannot be stopped from another thread".
    if (m_collector != nullptr && m_collector->thread() != QThread::currentThread()) {
        QMetaObject::invokeMethod(m_collector, "shutdown", Qt::BlockingQueuedConnection);
    }

    m_collectorThread->quit();
    m_collectorThread->wait();
    // Deleted outright rather than via the usual finished→deleteLater: once
    // wait() returns, the worker's event loop is gone, so a deferred delete has
    // nothing left to run it. The collector owns no timer by this point.
    delete m_collector;
    m_collector = nullptr;
    delete m_collectorThread;
    m_collectorThread = nullptr;

    // Peak claims "the last 60 s". Nothing is sampled while the dialog is
    // hidden, so a ring carried across that gap would describe a minute nobody
    // observed.
    m_ring.clear();
}

// ── Logs ────────────────────────────────────────────────────────────────────

QWidget* SystemInfoDialog::buildLogsTab()
{
    auto* page = new QWidget;
    m_logsPage = page;
    auto* layout = new QVBoxLayout(page);

    // Every enabled category gets a checkbox, and an operator with a dozen
    // categories on overflows any window. Scroll the row rather than clipping
    // it — a filter you cannot reach is the same as one that does not exist.
    auto* filterHost = new QWidget(page);
    m_filterRow = new QHBoxLayout(filterHost);
    m_filterRow->setContentsMargins(0, 0, 0, 0);
    m_filterScroll = new QScrollArea(page);
    m_filterScroll->setWidget(filterHost);
    m_filterScroll->setWidgetResizable(true);
    m_filterScroll->setFrameShape(QFrame::NoFrame);
    m_filterScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_filterScroll);

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
    m_logViewer = new QPlainTextEdit(page);
    m_logViewer->setReadOnly(true);
    m_logViewer->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_logViewer->setMaximumBlockCount(static_cast<int>(kMaxStoredLines));
    new LogSyntaxHighlighter(m_logViewer->document());
    layout->addWidget(m_logViewer, 1);

    rebuildCategoryFilters();
    return page;
}

void SystemInfoDialog::rebuildCategoryFilters()
{
    if (m_filterRow == nullptr || m_logsPage == nullptr) {
        return;   // called before the page exists; nothing to parent widgets to
    }

    // Snapshot which categories we already had boxes for BEFORE clearing them:
    // it is the only way to tell "new category, show it" from "the operator
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

    const QList<LogManager::Category> categories = LogManager::instance().categories();
    QSet<QString> stillEnabled;
    m_filterRow->addWidget(new QLabel(QStringLiteral("Show:"), m_logsPage));

    for (const LogManager::Category& category : categories) {
        if (!category.enabled) {
            continue;   // not being logged, so there is nothing to offer
        }
        stillEnabled.insert(category.id);

        auto* box = new QCheckBox(category.label, m_logsPage);
        box->setToolTip(QStringLiteral("%1 — %2").arg(category.id, category.description));
        // A category newly switched on starts visible; one the operator has
        // deliberately unticked here stays hidden across rebuilds.
        box->setChecked(previouslyKnown.contains(category.id)
                            ? m_enabledCategories.contains(category.id)
                            : true);
        if (box->isChecked()) {
            m_enabledCategories.insert(category.id);
        }
        const QString id = category.id;
        connect(box, &QCheckBox::toggled, this, [this, id](bool on) {
            if (on) {
                m_enabledCategories.insert(id);
            } else {
                m_enabledCategories.remove(id);
            }
            rebuildLogView();
        });
        m_categoryBoxes.insert(category.id, box);
        m_filterRow->addWidget(box);
    }

    if (m_categoryBoxes.isEmpty()) {
        auto* hint = new QLabel(
            QStringLiteral("No log categories are switched on — enable them in "
                           "Help \u2192 Support."),
            m_logsPage);
        m_filterRow->addWidget(hint);
    }

    // Drop view-filter state for categories that are no longer logged at all.
    m_enabledCategories.intersect(stillEnabled);
    m_filterRow->addStretch(1);

    // Size the viewport only now the row has widgets in it. Measuring the host
    // at construction time — before this function has ever run — reports the
    // height of an empty widget, and the scroll area collapses to nothing but
    // its own scrollbar.
    if (m_filterScroll != nullptr) {
        const QWidget* host = m_filterScroll->widget();
        m_filterScroll->setFixedHeight(host->sizeHint().height() + 18);
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
    if (m_logFile.isOpen()) {
        return;
    }
    const QString path = LogManager::instance().logFilePath();
    if (path.isEmpty()) {
        return;
    }
    m_logFile.setFileName(path);
    if (!m_logFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    // Start from the tail, not the beginning: a long session's log is tens of
    // megabytes and none of it is the stall being investigated right now.
    const qint64 size = m_logFile.size();
    if (size > kInitialTailBytes) {
        m_logFile.seek(size - kInitialTailBytes);
        m_logFile.readLine();  // discard the partial line the seek landed in
    }
    pollLog();

    if (m_logTimer == nullptr) {
        m_logTimer = new QTimer(this);
        m_logTimer->setInterval(kLogPollMs);
        connect(m_logTimer, &QTimer::timeout, this, &SystemInfoDialog::pollLog);
    }
    m_logTimer->start();
}

void SystemInfoDialog::closeLogTail()
{
    if (m_logTimer != nullptr) {
        m_logTimer->stop();
    }
    if (m_logFile.isOpen()) {
        m_logFile.close();
    }
}

void SystemInfoDialog::pollLog()
{
    if (!m_logFile.isOpen()) {
        return;
    }
    while (!m_logFile.atEnd()) {
        const QString line = QString::fromUtf8(m_logFile.readLine()).trimmed();
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
    if (m_logViewer == nullptr || !m_enabledCategories.contains(category)) {
        return;
    }
    m_logViewer->appendPlainText(line);
    m_logViewer->verticalScrollBar()->setValue(
        m_logViewer->verticalScrollBar()->maximum());
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
    m_logViewer->clear();
    for (const auto& entry : m_logLines) {
        if (m_enabledCategories.contains(entry.first)) {
            m_logViewer->appendPlainText(entry.second);
        }
    }
    m_logViewer->verticalScrollBar()->setValue(
        m_logViewer->verticalScrollBar()->maximum());
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
    closeLogTail();
    PersistentDialog::hideEvent(event);
}

}  // namespace AetherSDR
