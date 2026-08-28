#pragma once

#include "PersistentDialog.h"
#include "core/SystemInfo.h"

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

private:
    QWidget* buildThreadsTab();
    QWidget* buildLogsTab();

    void startSampling();
    void stopSampling();
    void applySample(const QVector<ThreadCpuSample>& threads);

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
    QThread*      m_collectorThread{nullptr};
    SystemInfoCollector* m_collector{nullptr};

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
