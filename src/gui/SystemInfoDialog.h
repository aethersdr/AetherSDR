#pragma once

#include "PersistentDialog.h"
#include "core/SystemInfo.h"
#include "core/ThreadCpuRing.h"

#include <QFile>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

class QCheckBox;
class QPushButton;
class QHBoxLayout;
class QLabel;
class QPlainTextEdit;
class QTableWidget;
class QThread;
class QTimer;

namespace AetherSDR {

class SystemInfoCollector;

// Runtime diagnostics for AetherSDR itself (#2554).
//
// Two tabs, and they are a pair rather than a list: Threads says WHICH thread is
// hot, Logs says what it was doing. The characteristic failure here is one
// thread saturating one core while the others idle (#2545), which the status
// bar's single system-wide percentage cannot show — but knowing a thread is at
// 98 % is only half a diagnosis without the log context beside it.
class SystemInfoDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit SystemInfoDialog(QWidget* parent = nullptr);
    ~SystemInfoDialog() override;

protected:
    // Sampling follows visibility. A thread enumeration every 1.5 s for the
    // life of the process would be observer effect on the thing being observed,
    // and this is the one place that policy is expressed.
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    // A slot, and named in the meta-object, so a test can drive a synthetic
    // sample all the way into the table with QMetaObject::invokeMethod. Every
    // defect this dialog shipped with was found by opening it rather than by
    // the suite, and the table's contents were the largest thing no test could
    // reach.
    void applySample(const QVector<AetherSDR::ThreadCpuSample>& threads);

    // Acceptance criterion 3, in its minimal form: the summary line goes red
    // when a thread crosses 90 % of one core. A slot for the same reason
    // applySample is one — a test can raise the alert without a machine that
    // can actually saturate a core on demand.
    void onThresholdExceeded(const QString& threadName, double percentOfCore);

    // A slot for the same reason as applySample: the two defects this tab
    // shipped with were both about which lines reach the view, and a test can
    // only pin that if it can hand the tab a line.
    void appendLogLine(const QString& line);

    // A slot so a test can step the tail deterministically instead of waiting
    // on the 500 ms timer — which is how the rotation path gets exercised.
    void pollLog();

private:
    QWidget* buildThreadsTab();
    QWidget* buildLogsTab();

    void applyAlertStyle();

    void startSampling();
    void stopSampling();

    void rebuildCategoryFilters();
    void openLogTail();
    void pauseLogTail();
    // Reopen after the file underneath us was rotated, restarted or replaced.
    // Returns false when there is nothing to follow.
    bool reopenLogTail(const QString& path);
    void rebuildLogView();
    // One place that owns the follow state, its button's text and tooltip, and
    // the jump to the newest line — so the button, the scrollbar and the
    // append path cannot end up disagreeing about whether we are following.
    void setLogFollowLive(bool on);
    static QString categoryFromLine(const QString& line);

    // Threads tab
    QTableWidget* m_threadTable{nullptr};
    QLabel*       m_threadSummary{nullptr};
    // Recent readings per thread, for the Peak column. Cleared when sampling
    // stops — see stopSampling().
    ThreadCpuRing m_ring;
    // Raised by the collector's crossing signal, cleared by the first sample
    // that comes back below the threshold. The signal is edge-triggered — one
    // event per crossing rather than one per sample — so the level it implies
    // has to be held here.
    bool    m_thresholdAlert{false};
    QString m_alertThreadName;
    double  m_alertPercent{0.0};
    QThread*      m_collectorThread{nullptr};
    SystemInfoCollector* m_collector{nullptr};
    // Bumped on every start AND stop. A sampleReady already queued to this
    // thread when stopSampling() runs is still delivered afterwards — Qt does
    // not withdraw posted calls when the sender dies — and would refill the
    // ring just cleared, or re-raise the alert on a hidden dialog. The
    // connections compare the generation they were made under and drop what
    // no longer belongs to a live sampling run.
    quint64       m_samplingGeneration{0};

    // Logs tab
    QWidget*        m_logsPage{nullptr};   // parent for dynamically rebuilt filters
    QPlainTextEdit* m_logViewer{nullptr};
    QHBoxLayout*    m_filterRow{nullptr};
    QPushButton*    m_logLiveToggle{nullptr};
    bool            m_logFollowLive{true};
    // Guards the scrollbar handler against our OWN scrolling: every jump to the
    // bottom fires valueChanged, and without this the first appended line would
    // look like the operator scrolling and switch following off.
    bool            m_handlingLogScroll{false};
    QLabel*         m_logPathLabel{nullptr};
    QTimer*         m_logTimer{nullptr};
    QFile           m_logFile;
    // Bytes after the last newline read so far. The writer flushes whole
    // lines, but a poll can still land between two writes of one batch; the
    // fragment waits here for its newline rather than being shown as a line
    // and its remainder filed under "default", which has no box.
    QByteArray      m_logPartialLine;
    QVector<QPair<QString, QString>> m_logLines;  // category, text
    QSet<QString>   m_enabledCategories;
    QHash<QString, QCheckBox*> m_categoryBoxes;
};

}  // namespace AetherSDR
