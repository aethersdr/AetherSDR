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
class QHBoxLayout;
class QLabel;
class QPlainTextEdit;
class QScrollArea;
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

private:
    QWidget* buildThreadsTab();
    QWidget* buildLogsTab();

    void applyAlertStyle();

    void startSampling();
    void stopSampling();

    void rebuildCategoryFilters();
    void openLogTail();
    void closeLogTail();
    void pollLog();
    void appendLogLine(const QString& line);
    void rebuildLogView();
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
    QScrollArea*    m_filterScroll{nullptr};
    QTimer*         m_logTimer{nullptr};
    QFile           m_logFile;
    QVector<QPair<QString, QString>> m_logLines;  // category, text
    QSet<QString>   m_enabledCategories;
    QHash<QString, QCheckBox*> m_categoryBoxes;
};

}  // namespace AetherSDR
