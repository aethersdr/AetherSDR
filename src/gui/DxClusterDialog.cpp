#include "DxClusterDialog.h"
#include "FramelessResizer.h"
#include "FramelessWindowTitleBar.h"
#include "GuardedSlider.h"
#include "core/DxClusterClient.h"
#include "core/AppSettings.h"
#include "core/SpotCommandPolicy.h"
#include "core/SpotModeResolver.h"
#include "models/RadioModel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QCheckBox>
#include <QGroupBox>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTabWidget>
#include <QTableView>
#include <QHeaderView>
#include <QSortFilterProxyModel>
#include <QSlider>
#include <QColorDialog>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTimer>

namespace AetherSDR {

// Shared DSP-style toggle for every checkable button in SpotHub
// (matches kDspToggle in VfoWidget.cpp so the chrome reads the same as
// the NB / NR / ANF buttons in the VFO panel).  Dark inset by default,
// green fill when checked, cyan hover-border accent.
static const QString kSpotHubToggle =
    "QPushButton { background: #1a2a3a; border: 1px solid #304050;"
    " border-radius: 2px; color: #c8d8e8; font-size: 13px;"
    " font-weight: bold; padding: 2px 8px; }"
    "QPushButton:checked { background: #1a6030; color: #ffffff;"
    " border: 1px solid #20a040; }"
    "QPushButton:hover { border: 1px solid #0090e0; }";

// Read the last N lines of a file without loading the entire thing.
static QStringList tailFile(const QString& path, int maxLines = 500)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    // For small files, just read everything
    if (f.size() < 64 * 1024) {
        QStringList lines;
        while (!f.atEnd()) {
            QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (!line.isEmpty())
                lines.append(line);
        }
        if (lines.size() > maxLines)
            lines = lines.mid(lines.size() - maxLines);
        return lines;
    }

    // For large files, seek backwards to find enough newlines
    constexpr qint64 CHUNK = 8192;
    qint64 pos = f.size();
    QByteArray tail;
    int nlCount = 0;
    while (pos > 0 && nlCount <= maxLines) {
        qint64 readSize = qMin(CHUNK, pos);
        pos -= readSize;
        f.seek(pos);
        QByteArray chunk = f.read(readSize);
        tail.prepend(chunk);
        nlCount += chunk.count('\n');
    }
    QStringList all = QString::fromUtf8(tail).split('\n', Qt::SkipEmptyParts);
    for (auto& s : all) s = s.trimmed();
    all.removeAll(QString());
    if (all.size() > maxLines)
        all = all.mid(all.size() - maxLines);
    return all;
}

// ── SpotTableModel ──────────────────────────────────────────────────────────

QString SpotTableModel::extractMode(const QString& comment)
{
    return SpotModeResolver::extractSpotModeFromComment(comment);
}

QVariant SpotTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_spots.size())
        return {};

    const auto& spot = m_spots[index.row()];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColTime:    return spot.utcTime.toString("HH:mm");
        case ColFreq:    return QString::number(spot.freqMhz * 1000.0, 'f', 1);
        case ColDxCall:  return spot.dxCall;
        case ColMode:    return extractMode(spot.comment);
        case ColComment: return spot.comment;
        case ColSpotter: return spot.spotterCall;
        case ColBand:    return bandForFreq(spot.freqMhz);
        case ColSource:  return spot.source;
        }
    }
    if (role == Qt::TextAlignmentRole) {
        if (index.column() == ColFreq)
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        if (index.column() == ColTime)
            return QVariant(Qt::AlignCenter);
    }
    if (role == Qt::ForegroundRole) {
        if (index.column() == ColDxCall)
            return QColor(0x00, 0xb4, 0xd8);  // accent
        if (index.column() == ColFreq)
            return QColor(0xe0, 0xd0, 0x60);  // yellow-ish
    }
    // Store freq in UserRole for sorting
    if (role == Qt::UserRole && index.column() == ColFreq)
        return spot.freqMhz;

    return {};
}

QVariant SpotTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case ColTime:    return "Time";
    case ColFreq:    return "Freq (kHz)";
    case ColDxCall:  return "DX Call";
    case ColMode:    return "Mode";
    case ColComment: return "Comment";
    case ColSpotter: return "Spotter";
    case ColBand:    return "Band";
    case ColSource:  return "Source";
    }
    return {};
}

void SpotTableModel::addSpot(const DxSpot& spot)
{
    beginInsertRows({}, 0, 0);
    m_spots.prepend(spot);
    endInsertRows();

    if (m_spots.size() > m_maxSpots) {
        beginRemoveRows({}, m_maxSpots, m_spots.size() - 1);
        m_spots.resize(m_maxSpots);
        endRemoveRows();
    }
}

void SpotTableModel::addSpots(const QVector<DxSpot>& spots)
{
    if (spots.isEmpty()) return;
    int count = spots.size();
    beginInsertRows({}, 0, count - 1);
    // Prepend in reverse so newest is at index 0
    for (int i = count - 1; i >= 0; --i)
        m_spots.prepend(spots[i]);
    endInsertRows();

    if (m_spots.size() > m_maxSpots) {
        beginRemoveRows({}, m_maxSpots, m_spots.size() - 1);
        m_spots.resize(m_maxSpots);
        endRemoveRows();
    }
}

const DxSpot* SpotTableModel::spotAt(int row) const
{
    if (row >= 0 && row < m_spots.size())
        return &m_spots[row];
    return nullptr;
}

void SpotTableModel::clear()
{
    beginResetModel();
    m_spots.clear();
    endResetModel();
}

QString SpotTableModel::bandForFreq(double mhz)
{
    if (mhz >= 1.8   && mhz <= 2.0)    return "160m";
    if (mhz >= 3.5   && mhz <= 4.0)    return "80m";
    if (mhz >= 5.0   && mhz <= 5.5)    return "60m";
    if (mhz >= 7.0   && mhz <= 7.3)    return "40m";
    if (mhz >= 10.1  && mhz <= 10.15)  return "30m";
    if (mhz >= 14.0  && mhz <= 14.35)  return "20m";
    if (mhz >= 18.068 && mhz <= 18.168) return "17m";
    if (mhz >= 21.0  && mhz <= 21.45)  return "15m";
    if (mhz >= 24.89 && mhz <= 24.99)  return "12m";
    if (mhz >= 28.0  && mhz <= 29.7)   return "10m";
    if (mhz >= 50.0  && mhz <= 54.0)   return "6m";
    if (mhz >= 144.0 && mhz <= 148.0)  return "2m";
    return "";
}

// ── BandFilterProxy ─────────────────────────────────────────────────────────

void BandFilterProxy::setBandVisible(const QString& band, bool visible)
{
    if (visible)
        m_hiddenBands.remove(band);
    else
        m_hiddenBands.insert(band);
    invalidateFilter();
}

bool BandFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    if (m_hiddenBands.isEmpty())
        return true;
    auto idx = sourceModel()->index(sourceRow, SpotTableModel::ColBand, sourceParent);
    QString band = sourceModel()->data(idx, Qt::DisplayRole).toString();
    if (band.isEmpty())
        return true;  // unknown band — always show
    return !m_hiddenBands.contains(band);
}

// ── DxClusterDialog ─────────────────────────────────────────────────────────

DxClusterDialog::DxClusterDialog(DxClusterClient* clusterClient, DxClusterClient* rbnClient,
                                   WsjtxClient* wsjtxClient, SpotCollectorClient* spotCollectorClient,
                                   PotaClient* potaClient,
#ifdef HAVE_WEBSOCKETS
                                   FreeDvClient* freedvClient,
#endif
                                   RadioModel* radioModel,
                                   DxccColorProvider* dxccProvider,
                                   QWidget* parent)
    : QDialog(parent), m_client(clusterClient), m_rbnClient(rbnClient),
      m_wsjtxClient(wsjtxClient), m_spotCollectorClient(spotCollectorClient),
      m_potaClient(potaClient),
#ifdef HAVE_WEBSOCKETS
      m_freedvClient(freedvClient),
#endif
      m_radioModel(radioModel), m_dxccProvider(dxccProvider)
{
    setWindowTitle("SpotHub");
    setMinimumSize(680, 560);
    resize(760, 640);

    // Outer layout hosts the frameless title bar + content area.  The
    // content area in turn carries the QTabWidget with the existing
    // body padding, so flipping frameless on/off only adjusts the
    // outer's top margin and the title bar's visibility.
    m_outerLayout = new QVBoxLayout(this);
    m_outerLayout->setSpacing(0);
    m_outerLayout->setContentsMargins(0, 0, 0, 0);

    m_titleBar = new FramelessWindowTitleBar(QStringLiteral("SpotHub"), this);
    m_outerLayout->addWidget(m_titleBar);

    auto* content = new QWidget(this);
    auto* root = new QVBoxLayout(content);
    root->setSpacing(0);
    root->setContentsMargins(4, 4, 4, 4);
    m_outerLayout->addWidget(content, 1);

    auto* tabs = new QTabWidget;
    tabs->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #203040; }"
        "QTabBar::tab { background: #1a1a2e; color: #808890; border: 1px solid #203040; "
        "  padding: 6px 16px; margin-right: 2px; }"
        "QTabBar::tab:selected { background: #0f0f1a; color: #00b4d8; border-bottom: none; }");

    buildClusterTab(tabs);
    buildRbnTab(tabs);
    buildWsjtxTab(tabs);
    buildSpotCollectorTab(tabs);
    buildPotaTab(tabs);
#ifdef HAVE_WEBSOCKETS
    buildFreeDvTab(tabs);
#endif
    buildSpotListTab(tabs);
    buildDisplayTab(tabs);

    root->addWidget(tabs);

    // 8-axis edge resize for the frameless mode.
    FramelessResizer::install(this);
    setFramelessMode(
        AppSettings::instance().value("FramelessWindow", "True").toString() == "True");

    // ── Spot batch timer (1/sec flush) ──────────────────────────────────
    m_spotBatchTimer = new QTimer(this);
    m_spotBatchTimer->start(1000);
    connect(m_spotBatchTimer, &QTimer::timeout, this, &DxClusterDialog::flushSpotBatch);

    // Auto-scroll helper: only scroll if user is already at the bottom
    auto isAtBottom = [](QAbstractScrollArea* w) {
        auto* sb = w->verticalScrollBar();
        return sb->value() >= sb->maximum() - 2;
    };

    // ── Live updates from client ────────────────────────────────────────
    connect(clusterClient, &DxClusterClient::rawLineReceived, this, [this, isAtBottom](const QString& line) {
        bool follow = isAtBottom(m_console);
        m_console->appendPlainText(line);
        if (follow) {
            auto* sb = m_console->verticalScrollBar();
            sb->setValue(sb->maximum());
        }
    });

    connect(clusterClient, &DxClusterClient::spotReceived, this, [this](DxSpot spot) {
        spot.source = "Cluster";
        m_spotBatch.append(spot);
    });

    connect(clusterClient, &DxClusterClient::connected, this, [this] {
        m_statusLabel->setText(QString("Connected to %1:%2").arg(m_client->host()).arg(m_client->port()));
        m_statusLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 11px; }");
        m_connectBtn->setText("Disconnect");
        m_cmdEdit->setEnabled(true);
        m_sendBtn->setEnabled(true);
        m_console->appendPlainText("--- Connected ---");
    });
    connect(clusterClient, &DxClusterClient::disconnected, this, [this] {
        m_statusLabel->setText("Disconnected");
        m_statusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
        m_connectBtn->setText("Connect");
        m_cmdEdit->setEnabled(false);
        m_sendBtn->setEnabled(false);
        m_console->appendPlainText("--- Disconnected ---");
    });
    connect(clusterClient, &DxClusterClient::connectionError, this, [this](const QString& err) {
        m_statusLabel->setText("Error: " + err);
        m_statusLabel->setStyleSheet("QLabel { color: #ff4444; font-size: 11px; }");
        m_console->appendPlainText("--- Error: " + err + " ---");
    });

    // Defer log file loading until after the dialog is shown (#748).
    // Reads only the last 500 lines per file to avoid blocking on large logs.
    auto clusterLogPath = clusterClient->logFilePath();
    auto rbnLogPath = rbnClient->logFilePath();
    auto wsjtxLogPath = wsjtxClient->logFilePath();
    auto potaLogPath = potaClient->logFilePath();
    QString freedvLogPath;
#ifdef HAVE_WEBSOCKETS
    freedvLogPath = freedvClient->logFilePath();
#endif
    QTimer::singleShot(0, this, [this, clusterLogPath, rbnLogPath,
                                  wsjtxLogPath, potaLogPath, freedvLogPath]() {
        loadLogFiles(clusterLogPath, rbnLogPath, wsjtxLogPath, potaLogPath, freedvLogPath);
    });

    // ── Live updates from RBN client ──────────────────────────────────
    connect(rbnClient, &DxClusterClient::rawLineReceived, this, [this, isAtBottom](const QString& line) {
        bool follow = isAtBottom(m_rbnConsole);
        m_rbnConsole->appendPlainText(line);
        if (follow) {
            auto* sb = m_rbnConsole->verticalScrollBar();
            sb->setValue(sb->maximum());
        }
    });

    connect(rbnClient, &DxClusterClient::spotReceived, this, [this](DxSpot spot) {
        spot.source = "RBN";
        m_spotBatch.append(spot);
    });

    connect(rbnClient, &DxClusterClient::connected, this, [this] {
        m_rbnStatusLabel->setText(QString("Connected to %1:%2").arg(m_rbnClient->host()).arg(m_rbnClient->port()));
        m_rbnStatusLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 11px; }");
        m_rbnConnectBtn->setText("Disconnect");
        m_rbnCmdEdit->setEnabled(true);
        m_rbnSendBtn->setEnabled(true);
        m_rbnConsole->appendPlainText("--- Connected ---");
    });
    connect(rbnClient, &DxClusterClient::disconnected, this, [this] {
        m_rbnStatusLabel->setText("Disconnected");
        m_rbnStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
        m_rbnConnectBtn->setText("Connect");
        m_rbnCmdEdit->setEnabled(false);
        m_rbnSendBtn->setEnabled(false);
        m_rbnConsole->appendPlainText("--- Disconnected ---");
    });
    connect(rbnClient, &DxClusterClient::connectionError, this, [this](const QString& err) {
        m_rbnStatusLabel->setText("Error: " + err);
        m_rbnStatusLabel->setStyleSheet("QLabel { color: #ff4444; font-size: 11px; }");
        m_rbnConsole->appendPlainText("--- Error: " + err + " ---");
    });

    // RBN log loaded in deferred loadLogFiles() (#748)

    // ── Live updates from WSJT-X client ───────────────────────────────
    connect(wsjtxClient, &WsjtxClient::rawLineReceived, this, [this, isAtBottom](const QString& line) {
        bool follow = isAtBottom(m_wsjtxConsole);
        m_wsjtxConsole->appendPlainText(line);
        if (follow) {
            auto* sb = m_wsjtxConsole->verticalScrollBar();
            sb->setValue(sb->maximum());
        }
    });

    connect(wsjtxClient, &WsjtxClient::spotReceived, this, [this](DxSpot spot) {
        spot.source = "WSJT-X";
        // Apply spot filters:
        // - Nothing checked: everything passes
        // - CQ checked: only "CQ ..." messages
        // - CQ POTA checked: only "CQ POTA ..." messages
        // - Calling Me checked: only directed messages to my callsign
        // CQ and CQ POTA are mutually exclusive; Calling Me can combine with either
        const QString& msg = spot.comment;
        auto& as = AppSettings::instance();
        bool anyFilter = m_wsjtxFilterCQ->isChecked() || m_wsjtxFilterPOTA->isChecked()
                        || m_wsjtxFilterCallingMe->isChecked();

        // Determine which category matches and assign color
        bool isCQ = msg.startsWith("CQ ");
        bool isPOTA = msg.contains("CQ POTA");
        bool isCallingMe = false;
        {
            QString myCall = as.value("DxClusterCallsign").toString();
            if (!myCall.isEmpty()) {
                QStringList parts = msg.split(' ', Qt::SkipEmptyParts);
                if (parts.size() >= 2 && parts[0] == myCall)
                    isCallingMe = true;
            }
        }

        // Apply color always: Calling Me > POTA > CQ > default
        if (isCallingMe)
            spot.color = as.value("WsjtxColorCallingMe", "#FF0000").toString();
        else if (isPOTA)
            spot.color = as.value("WsjtxColorPOTA", "#00FFFF").toString();
        else if (isCQ)
            spot.color = as.value("WsjtxColorCQ", "#00FF00").toString();
        else
            spot.color = as.value("WsjtxColorDefault", "#FFFFFF").toString();

        // Filter: if any checkbox is checked, only matching spots pass
        if (anyFilter) {
            bool pass = false;
            if (m_wsjtxFilterCQ->isChecked() && isCQ) pass = true;
            if (m_wsjtxFilterPOTA->isChecked() && isPOTA) pass = true;
            if (m_wsjtxFilterCallingMe->isChecked() && isCallingMe) pass = true;
            if (!pass) return;
        }
        m_spotBatch.append(spot);
        emit wsjtxSpotFiltered(spot);
    });

    connect(wsjtxClient, &WsjtxClient::listening, this, [this] {
        m_wsjtxStatusLabel->setText(QString("Listening on port %1").arg(m_wsjtxPortSpin->value()));
        m_wsjtxStatusLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 11px; }");
        m_wsjtxStartBtn->setText("Stop");
        m_wsjtxConsole->appendPlainText("--- Listening ---");
    });
    connect(wsjtxClient, &WsjtxClient::stopped, this, [this] {
        m_wsjtxStatusLabel->setText("Stopped");
        m_wsjtxStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
        m_wsjtxStartBtn->setText("Start");
        m_wsjtxConsole->appendPlainText("--- Stopped ---");
    });

    // WSJT-X log loaded in deferred loadLogFiles() (#748)

    // ── Live updates from SpotCollector client ───────────────────────
    connect(spotCollectorClient, &SpotCollectorClient::rawLineReceived, this, [this, isAtBottom](const QString& line) {
        bool follow = isAtBottom(m_scConsole);
        m_scConsole->appendPlainText(line);
        if (follow) {
            auto* sb = m_scConsole->verticalScrollBar();
            sb->setValue(sb->maximum());
        }
    });

    connect(spotCollectorClient, &SpotCollectorClient::spotReceived, this, [this](DxSpot spot) {
        spot.source = "SpotCollector";
        m_spotBatch.append(spot);
    });

    connect(spotCollectorClient, &SpotCollectorClient::listening, this, [this] {
        m_scStatusLabel->setText(QString("Listening on port %1").arg(m_scPortSpin->value()));
        m_scStatusLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 11px; }");
        m_scStartBtn->setText("Stop");
        m_scConsole->appendPlainText("--- Listening ---");
    });
    connect(spotCollectorClient, &SpotCollectorClient::stopped, this, [this] {
        m_scStatusLabel->setText("Stopped");
        m_scStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
        m_scStartBtn->setText("Start");
        m_scConsole->appendPlainText("--- Stopped ---");
    });

    // ── Live updates from POTA client ─────────────────────────────────
    connect(potaClient, &PotaClient::rawLineReceived, this, [this, isAtBottom](const QString& line) {
        bool follow = isAtBottom(m_potaConsole);
        m_potaConsole->appendPlainText(line);
        if (follow) {
            auto* sb = m_potaConsole->verticalScrollBar();
            sb->setValue(sb->maximum());
        }
    });

    connect(potaClient, &PotaClient::spotReceived, this, [this](DxSpot spot) {
        spot.source = "POTA";
        m_spotBatch.append(spot);
    });

    connect(potaClient, &PotaClient::started, this, [this] {
        m_potaStatusLabel->setText("Polling...");
        m_potaStatusLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 11px; }");
        m_potaStartBtn->setText("Stop");
        m_potaConsole->appendPlainText("--- Polling started ---");
    });
    connect(potaClient, &PotaClient::stopped, this, [this] {
        m_potaStatusLabel->setText("Stopped");
        m_potaStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
        m_potaStartBtn->setText("Start");
        m_potaConsole->appendPlainText("--- Stopped ---");
    });
    connect(potaClient, &PotaClient::pollError, this, [this](const QString& err) {
        m_potaConsole->appendPlainText("--- Error: " + err + " ---");
    });
    connect(potaClient, &PotaClient::pollComplete, this, [this](int total, int newCount) {
        m_potaStatusLabel->setText(QString("Polling... (%1 active, %2 new)").arg(total).arg(newCount));
    });

    // POTA log loaded in deferred loadLogFiles() (#748)

#ifdef HAVE_WEBSOCKETS
    // ── Live updates from FreeDV client ───────────────────────────────
    connect(freedvClient, &FreeDvClient::rawLineReceived, this, [this, isAtBottom](const QString& line) {
        bool follow = isAtBottom(m_freedvConsole);
        m_freedvConsole->appendPlainText(line);
        if (follow) {
            auto* sb = m_freedvConsole->verticalScrollBar();
            sb->setValue(sb->maximum());
        }
    });
    connect(freedvClient, &FreeDvClient::spotReceived, this, [this](DxSpot spot) {
        spot.source = "FreeDV";
        m_spotBatch.append(spot);
    });
    connect(freedvClient, &FreeDvClient::started, this, [this] {
        m_freedvStatusLabel->setText("Connecting...");
        m_freedvStatusLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 11px; }");
        m_freedvStartBtn->setText("Stop");
        m_freedvConsole->appendPlainText("--- Connecting ---");
    });
    connect(freedvClient, &FreeDvClient::stopped, this, [this] {
        m_freedvStatusLabel->setText("Stopped");
        m_freedvStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
        m_freedvStartBtn->setText("Start");
        m_freedvConsole->appendPlainText("--- Stopped ---");
    });
    connect(freedvClient, &FreeDvClient::connectionError, this, [this](const QString& err) {
        m_freedvConsole->appendPlainText("--- Error: " + err + " ---");
    });

    // Update status when FreeDV connects via Socket.IO
    connect(freedvClient, &FreeDvClient::rawLineReceived, this, [this](const QString& line) {
        if (line.startsWith("Connected to")) {
            m_freedvStatusLabel->setText("Connected");
            m_freedvStatusLabel->setStyleSheet("QLabel { color: #30d050; font-size: 11px; }");
        }
    });

    // FreeDV log loaded in deferred loadLogFiles() (#748)
#endif

    // Scroll spot table to show newest entries
    m_spotTable->scrollToBottom();

    // Disable autoDefault on all buttons so Enter in command inputs
    // only fires returnPressed, not random button clicks (#459)
    for (auto* btn : findChildren<QPushButton*>())
        btn->setAutoDefault(false);

    updateStatus();
}

void DxClusterDialog::loadLogFiles(const QString& clusterLog, const QString& rbnLog,
                                    const QString& wsjtxLog, const QString& potaLog,
                                    const QString& freedvLog)
{
    static const QRegularExpression rx(
        R"(^DX\s+de\s+(\S+?):\s+(\d+\.?\d*)\s+(\S+)\s+(.*?)\s+(\d{4})Z)",
        QRegularExpression::CaseInsensitiveOption);

    auto parseSpots = [&](const QStringList& lines, const QString& source) {
        QVector<DxSpot> spots;
        for (const auto& line : lines) {
            auto match = rx.match(line);
            if (match.hasMatch()) {
                DxSpot spot;
                spot.spotterCall = match.captured(1);
                spot.freqMhz = match.captured(2).toDouble() / 1000.0;
                spot.dxCall = match.captured(3);
                spot.comment = match.captured(4).trimmed();
                QString timeStr = match.captured(5);
                spot.utcTime = QTime(timeStr.left(2).toInt(), timeStr.mid(2, 2).toInt());
                if (spot.freqMhz > 0.0 && !spot.dxCall.isEmpty()) {
                    spot.source = source;
                    spots.append(spot);
                }
            }
        }
        return spots;
    };

    auto loadConsole = [](QPlainTextEdit* console, const QStringList& lines) {
        if (!console || lines.isEmpty()) return;
        console->setPlainText(lines.join('\n'));
        auto* sb = console->verticalScrollBar();
        sb->setValue(sb->maximum());
    };

    // Cluster log — parse spots + display in console
    auto clusterLines = tailFile(clusterLog);
    loadConsole(m_console, clusterLines);
    auto clusterSpots = parseSpots(clusterLines, "Cluster");

    // RBN log — parse spots + display in console
    auto rbnLines = tailFile(rbnLog);
    loadConsole(m_rbnConsole, rbnLines);
    auto rbnSpots = parseSpots(rbnLines, "RBN");

    // WSJT-X log — display only (no DX de format)
    loadConsole(m_wsjtxConsole, tailFile(wsjtxLog));

    // POTA log — display only
    loadConsole(m_potaConsole, tailFile(potaLog));

    // FreeDV log — display only
#ifdef HAVE_WEBSOCKETS
    if (!freedvLog.isEmpty())
        loadConsole(m_freedvConsole, tailFile(freedvLog));
#else
    Q_UNUSED(freedvLog);
#endif

    // Batch all spots into the model at once
    QVector<DxSpot> allSpots;
    allSpots.reserve(clusterSpots.size() + rbnSpots.size());
    allSpots.append(clusterSpots);
    allSpots.append(rbnSpots);
    if (!allSpots.isEmpty())
        m_spotModel->addSpots(allSpots);

    m_spotTable->scrollToBottom();
}

void DxClusterDialog::buildClusterTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setSpacing(8);

    auto& s = AppSettings::instance();

    // ── Connection settings ─────────────────────────────────────────────
    auto* connGroup = new QGroupBox("Connection");
    auto* connLayout = new QVBoxLayout(connGroup);
    connLayout->setSpacing(4);

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    int row = 0;

    grid->addWidget(new QLabel("Server:"), row, 0);
    m_hostEdit = new QLineEdit(s.value("DxClusterHost", "dxc.nc7j.com").toString());
    m_hostEdit->setPlaceholderText("dxc.nc7j.com");
    m_hostEdit->setStyleSheet("QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    grid->addWidget(m_hostEdit, row, 1);
    row++;

    grid->addWidget(new QLabel("Port:"), row, 0);
    m_portSpin = new QSpinBox;
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(s.value("DxClusterPort", 7300).toInt());
    m_portSpin->setStyleSheet("QSpinBox { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    grid->addWidget(m_portSpin, row, 1);
    row++;

    grid->addWidget(new QLabel("Callsign:"), row, 0);
    m_callEdit = new QLineEdit(s.value("DxClusterCallsign").toString());
    m_callEdit->setPlaceholderText("your callsign");
    m_callEdit->setStyleSheet("QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    grid->addWidget(m_callEdit, row, 1);
    row++;

    connLayout->addLayout(grid);

    // Button row
    auto* btnRow = new QHBoxLayout;
    m_autoConnectBtn = new QPushButton(
        s.value("DxClusterAutoConnect", "False").toString() == "True" ? "Auto-Connect: ON" : "Auto-Connect: OFF");
    m_autoConnectBtn->setCheckable(true);
    m_autoConnectBtn->setChecked(s.value("DxClusterAutoConnect", "False").toString() == "True");
    m_autoConnectBtn->setStyleSheet(
        kSpotHubToggle);
    connect(m_autoConnectBtn, &QPushButton::toggled, this, [this](bool on) {
        m_autoConnectBtn->setText(on ? "Auto-Connect: ON" : "Auto-Connect: OFF");
        auto& s = AppSettings::instance();
        s.setValue("DxClusterAutoConnect", on ? "True" : "False");
        s.save();
    });
    btnRow->addWidget(m_autoConnectBtn);
    btnRow->addStretch();

    m_statusLabel = new QLabel("Disconnected");
    m_statusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
    btnRow->addWidget(m_statusLabel);
    btnRow->addStretch();

    m_connectBtn = new QPushButton(m_client->isConnected() ? "Disconnect" : "Connect");
    m_connectBtn->setFixedWidth(100);
    m_connectBtn->setStyleSheet(
        "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold; "
        "border: 1px solid #008ba8; padding: 4px; border-radius: 3px; }"
        "QPushButton:hover { background: #00c8f0; }"
        "QPushButton:disabled { background: #404060; color: #808080; }");
    connect(m_connectBtn, &QPushButton::clicked, this, [this] {
        if (m_client->isConnected()) {
            emit disconnectRequested();
            return;
        }
        QString host = m_hostEdit->text().trimmed();
        QString call = m_callEdit->text().trimmed().toUpper();
        quint16 port = static_cast<quint16>(m_portSpin->value());
        if (host.isEmpty() || call.isEmpty()) {
            m_statusLabel->setText("Server and callsign are required");
            m_statusLabel->setStyleSheet("QLabel { color: #ff4444; font-size: 11px; }");
            return;
        }
        auto& s = AppSettings::instance();
        s.setValue("DxClusterHost", host);
        s.setValue("DxClusterPort", port);
        s.setValue("DxClusterCallsign", call);
        s.save();
        emit connectRequested(host, port, call);
    });
    btnRow->addWidget(m_connectBtn);
    connLayout->addLayout(btnRow);

    layout->addWidget(connGroup);

    // ── Console output ──────────────────────────────────────────────────
    auto* consoleRow = new QHBoxLayout;
    auto* consoleLabel = new QLabel("Cluster Console");
    consoleLabel->setStyleSheet("QLabel { color: #00b4d8; font-weight: bold; }");
    consoleRow->addWidget(consoleLabel);
    consoleRow->addStretch();

    auto* dxcColorLabel = new QLabel("Spot Color:");
    dxcColorLabel->setStyleSheet("QLabel { color: #808080; font-size: 12px; }");
    consoleRow->addWidget(dxcColorLabel);

    QColor dxcColor(s.value("DxClusterSpotColor", "#D2B48C").toString());
    auto* dxcColorBtn = new QPushButton;
    dxcColorBtn->setFixedSize(18, 18);
    dxcColorBtn->setStyleSheet(QString(
        "QPushButton { background: %1; border: 2px solid #405060; border-radius: 3px; }"
        "QPushButton:hover { border-color: #c8d8e8; }").arg(dxcColor.name()));
    connect(dxcColorBtn, &QPushButton::clicked, this, [this, dxcColorBtn] {
        QColor c = QColorDialog::getColor(
            QColor(AppSettings::instance().value("DxClusterSpotColor", "#D2B48C").toString()),
            this, "DX Cluster Spot Color");
        if (c.isValid()) {
            dxcColorBtn->setStyleSheet(QString(
                "QPushButton { background: %1; border: 2px solid #405060; border-radius: 3px; }"
                "QPushButton:hover { border-color: #c8d8e8; }").arg(c.name()));
            AppSettings::instance().setValue("DxClusterSpotColor", c.name());
            AppSettings::instance().save();
        }
    });
    consoleRow->addWidget(dxcColorBtn);
    layout->addLayout(consoleRow);

    m_console = new QPlainTextEdit;
    m_console->setReadOnly(true);
    m_console->setMaximumBlockCount(2000);
    m_console->setStyleSheet(
        "QPlainTextEdit {"
        "  background: #0a0a14;"
        "  color: #a0b0c0;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "  border: 1px solid #203040;"
        "  padding: 4px;"
        "}");
    layout->addWidget(m_console, 1);

    // Command input row
    auto* cmdRow = new QHBoxLayout;
    m_cmdEdit = new QLineEdit;
    m_cmdEdit->setPlaceholderText("Type a cluster command (e.g. sh/dx 20, set/filter, bye)");
    m_cmdEdit->setStyleSheet("QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; font-family: monospace; }");
    m_cmdEdit->setEnabled(m_client->isConnected());
    connect(m_cmdEdit, &QLineEdit::returnPressed, this, [this] {
        QString cmd = m_cmdEdit->text().trimmed();
        if (cmd.isEmpty() || !m_client->isConnected()) return;
        QMetaObject::invokeMethod(m_client, [client=m_client, cmd] { client->sendCommand(cmd); });
        m_console->appendPlainText("> " + cmd);
        m_cmdEdit->clear();
    });
    m_sendBtn = new QPushButton("Send");
    m_sendBtn->setFixedWidth(60);
    m_sendBtn->setEnabled(m_client->isConnected());
    connect(m_sendBtn, &QPushButton::clicked, this, [this] {
        m_cmdEdit->returnPressed();
    });
    cmdRow->addWidget(m_cmdEdit, 1);
    cmdRow->addWidget(m_sendBtn);
    layout->addLayout(cmdRow);

    tabs->addTab(page, "Cluster");
}

void DxClusterDialog::buildRbnTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setSpacing(8);

    auto& s = AppSettings::instance();
    QString defaultCall = s.value("RbnCallsign").toString();
    if (defaultCall.isEmpty())
        defaultCall = s.value("DxClusterCallsign").toString();

    // ── Connection settings ─────────────────────────────────────────────
    auto* connGroup = new QGroupBox("RBN Connection");
    auto* connLayout = new QVBoxLayout(connGroup);
    connLayout->setSpacing(4);

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    int row = 0;

    grid->addWidget(new QLabel("Server:"), row, 0);
    m_rbnHostEdit = new QLineEdit(s.value("RbnHost", "telnet.reversebeacon.net").toString());
    m_rbnHostEdit->setPlaceholderText("telnet.reversebeacon.net");
    m_rbnHostEdit->setStyleSheet("QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    grid->addWidget(m_rbnHostEdit, row, 1);
    row++;

    grid->addWidget(new QLabel("Port:"), row, 0);
    m_rbnPortSpin = new QSpinBox;
    m_rbnPortSpin->setRange(1, 65535);
    m_rbnPortSpin->setValue(s.value("RbnPort", 7000).toInt());
    m_rbnPortSpin->setStyleSheet("QSpinBox { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    grid->addWidget(m_rbnPortSpin, row, 1);
    row++;

    grid->addWidget(new QLabel("Callsign:"), row, 0);
    m_rbnCallEdit = new QLineEdit(defaultCall);
    m_rbnCallEdit->setPlaceholderText("your callsign");
    m_rbnCallEdit->setStyleSheet("QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    grid->addWidget(m_rbnCallEdit, row, 1);
    row++;

    // Rate limit
    grid->addWidget(new QLabel("Rate Limit:"), row, 0);
    auto* rateRow = new QHBoxLayout;
    auto* rateSpin = new QSpinBox;
    rateSpin->setRange(1, 100);
    rateSpin->setValue(s.value("RbnRateLimit", 10).toInt());
    rateSpin->setSuffix(" spots/sec");
    rateSpin->setStyleSheet("QSpinBox { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    connect(rateSpin, &QSpinBox::valueChanged, this, [](int v) {
        auto& s = AppSettings::instance();
        s.setValue("RbnRateLimit", v);
        s.save();
    });
    rateRow->addWidget(rateSpin);
    rateRow->addStretch();
    grid->addLayout(rateRow, row, 1);
    row++;

    connLayout->addLayout(grid);

    // Button row
    auto* btnRow = new QHBoxLayout;
    m_rbnAutoConnectBtn = new QPushButton(
        s.value("RbnAutoConnect", "False").toString() == "True" ? "Auto-Connect: ON" : "Auto-Connect: OFF");
    m_rbnAutoConnectBtn->setCheckable(true);
    m_rbnAutoConnectBtn->setChecked(s.value("RbnAutoConnect", "False").toString() == "True");
    m_rbnAutoConnectBtn->setStyleSheet(
        kSpotHubToggle);
    connect(m_rbnAutoConnectBtn, &QPushButton::toggled, this, [this](bool on) {
        m_rbnAutoConnectBtn->setText(on ? "Auto-Connect: ON" : "Auto-Connect: OFF");
        auto& s = AppSettings::instance();
        s.setValue("RbnAutoConnect", on ? "True" : "False");
        s.save();
    });
    btnRow->addWidget(m_rbnAutoConnectBtn);
    btnRow->addStretch();

    m_rbnStatusLabel = new QLabel("Disconnected");
    m_rbnStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
    btnRow->addWidget(m_rbnStatusLabel);
    btnRow->addStretch();

    m_rbnConnectBtn = new QPushButton(m_rbnClient->isConnected() ? "Disconnect" : "Connect");
    m_rbnConnectBtn->setFixedWidth(100);
    m_rbnConnectBtn->setStyleSheet(
        "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold; "
        "border: 1px solid #008ba8; padding: 4px; border-radius: 3px; }"
        "QPushButton:hover { background: #00c8f0; }"
        "QPushButton:disabled { background: #404060; color: #808080; }");
    connect(m_rbnConnectBtn, &QPushButton::clicked, this, [this] {
        if (m_rbnClient->isConnected()) {
            emit rbnDisconnectRequested();
            return;
        }
        QString host = m_rbnHostEdit->text().trimmed();
        QString call = m_rbnCallEdit->text().trimmed().toUpper();
        quint16 port = static_cast<quint16>(m_rbnPortSpin->value());
        if (host.isEmpty() || call.isEmpty()) {
            m_rbnStatusLabel->setText("Server and callsign are required");
            m_rbnStatusLabel->setStyleSheet("QLabel { color: #ff4444; font-size: 11px; }");
            return;
        }
        auto& s = AppSettings::instance();
        s.setValue("RbnHost", host);
        s.setValue("RbnPort", port);
        s.setValue("RbnCallsign", call);
        s.save();
        emit rbnConnectRequested(host, port, call);
    });
    btnRow->addWidget(m_rbnConnectBtn);
    connLayout->addLayout(btnRow);

    layout->addWidget(connGroup);

    // ── Console output ──────────────────────────────────────────────────
    auto* rbnConsoleRow = new QHBoxLayout;
    auto* rbnConsoleLabel = new QLabel("RBN Console");
    rbnConsoleLabel->setStyleSheet("QLabel { color: #00b4d8; font-weight: bold; }");
    rbnConsoleRow->addWidget(rbnConsoleLabel);
    rbnConsoleRow->addStretch();

    auto* rbnColorLabel = new QLabel("Spot Color:");
    rbnColorLabel->setStyleSheet("QLabel { color: #808080; font-size: 12px; }");
    rbnConsoleRow->addWidget(rbnColorLabel);

    QColor rbnColor(s.value("RbnSpotColor", "#4488FF").toString());
    auto* rbnColorBtn = new QPushButton;
    rbnColorBtn->setFixedSize(18, 18);
    rbnColorBtn->setStyleSheet(QString(
        "QPushButton { background: %1; border: 2px solid #405060; border-radius: 3px; }"
        "QPushButton:hover { border-color: #c8d8e8; }").arg(rbnColor.name()));
    connect(rbnColorBtn, &QPushButton::clicked, this, [this, rbnColorBtn] {
        QColor c = QColorDialog::getColor(
            QColor(AppSettings::instance().value("RbnSpotColor", "#4488FF").toString()),
            this, "RBN Spot Color");
        if (c.isValid()) {
            rbnColorBtn->setStyleSheet(QString(
                "QPushButton { background: %1; border: 2px solid #405060; border-radius: 3px; }"
                "QPushButton:hover { border-color: #c8d8e8; }").arg(c.name()));
            AppSettings::instance().setValue("RbnSpotColor", c.name());
            AppSettings::instance().save();
        }
    });
    rbnConsoleRow->addWidget(rbnColorBtn);
    layout->addLayout(rbnConsoleRow);

    m_rbnConsole = new QPlainTextEdit;
    m_rbnConsole->setReadOnly(true);
    m_rbnConsole->setMaximumBlockCount(2000);
    m_rbnConsole->setStyleSheet(
        "QPlainTextEdit {"
        "  background: #0a0a14;"
        "  color: #a0b0c0;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "  border: 1px solid #203040;"
        "  padding: 4px;"
        "}");
    layout->addWidget(m_rbnConsole, 1);

    // Command input row
    auto* cmdRow = new QHBoxLayout;
    m_rbnCmdEdit = new QLineEdit;
    m_rbnCmdEdit->setPlaceholderText("Type an RBN command (e.g. set/skimmer, set/ft8, bye)");
    m_rbnCmdEdit->setStyleSheet("QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; font-family: monospace; }");
    m_rbnCmdEdit->setEnabled(m_rbnClient->isConnected());
    connect(m_rbnCmdEdit, &QLineEdit::returnPressed, this, [this] {
        QString cmd = m_rbnCmdEdit->text().trimmed();
        if (cmd.isEmpty() || !m_rbnClient->isConnected()) return;
        QMetaObject::invokeMethod(m_rbnClient, [client=m_rbnClient, cmd] { client->sendCommand(cmd); });
        m_rbnConsole->appendPlainText("> " + cmd);
        m_rbnCmdEdit->clear();
    });
    m_rbnSendBtn = new QPushButton("Send");
    m_rbnSendBtn->setFixedWidth(60);
    m_rbnSendBtn->setEnabled(m_rbnClient->isConnected());
    connect(m_rbnSendBtn, &QPushButton::clicked, this, [this] {
        m_rbnCmdEdit->returnPressed();
    });
    cmdRow->addWidget(m_rbnCmdEdit, 1);
    cmdRow->addWidget(m_rbnSendBtn);
    layout->addLayout(cmdRow);

    tabs->addTab(page, "RBN");
}

void DxClusterDialog::buildWsjtxTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setSpacing(8);

    auto& s = AppSettings::instance();

    // ── Connection settings ─────────────────────────────────────────────
    auto* connGroup = new QGroupBox("WSJT-X Listener Address");
    auto* connLayout = new QVBoxLayout(connGroup);
    connLayout->setSpacing(4);

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    int row = 0;

    grid->addWidget(new QLabel("Address:"), row, 0);
    m_wsjtxAddrEdit = new QLineEdit(s.value("WsjtxAddress", "224.0.0.1").toString());
    m_wsjtxAddrEdit->setPlaceholderText("224.0.0.1 (multicast) or 0.0.0.0 (any)");
    m_wsjtxAddrEdit->setStyleSheet("QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    grid->addWidget(m_wsjtxAddrEdit, row, 1);
    row++;

    grid->addWidget(new QLabel("Port:"), row, 0);
    m_wsjtxPortSpin = new QSpinBox;
    m_wsjtxPortSpin->setRange(1, 65535);
    m_wsjtxPortSpin->setValue(s.value("WsjtxPort", 2237).toInt());
    m_wsjtxPortSpin->setStyleSheet("QSpinBox { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    grid->addWidget(m_wsjtxPortSpin, row, 1);
    row++;

    connLayout->addLayout(grid);

    // Button row
    auto* btnRow = new QHBoxLayout;
    m_wsjtxAutoStartBtn = new QPushButton(
        s.value("WsjtxAutoStart", "False").toString() == "True" ? "Auto-Start: ON" : "Auto-Start: OFF");
    m_wsjtxAutoStartBtn->setCheckable(true);
    m_wsjtxAutoStartBtn->setChecked(s.value("WsjtxAutoStart", "False").toString() == "True");
    m_wsjtxAutoStartBtn->setStyleSheet(
        kSpotHubToggle);
    connect(m_wsjtxAutoStartBtn, &QPushButton::toggled, this, [this](bool on) {
        m_wsjtxAutoStartBtn->setText(on ? "Auto-Start: ON" : "Auto-Start: OFF");
        auto& s = AppSettings::instance();
        s.setValue("WsjtxAutoStart", on ? "True" : "False");
        s.save();
    });
    btnRow->addWidget(m_wsjtxAutoStartBtn);
    btnRow->addStretch();

    m_wsjtxStatusLabel = new QLabel("Stopped");
    m_wsjtxStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
    btnRow->addWidget(m_wsjtxStatusLabel);
    btnRow->addStretch();

    m_wsjtxStartBtn = new QPushButton(m_wsjtxClient->isListening() ? "Stop" : "Start");
    m_wsjtxStartBtn->setFixedWidth(100);
    m_wsjtxStartBtn->setStyleSheet(
        "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold; "
        "border: 1px solid #008ba8; padding: 4px; border-radius: 3px; }"
        "QPushButton:hover { background: #00c8f0; }"
        "QPushButton:disabled { background: #404060; color: #808080; }");
    connect(m_wsjtxStartBtn, &QPushButton::clicked, this, [this] {
        if (m_wsjtxClient->isListening()) {
            emit wsjtxStopRequested();
            return;
        }
        quint16 port = static_cast<quint16>(m_wsjtxPortSpin->value());
        QString addr = m_wsjtxAddrEdit->text().trimmed();
        if (addr.isEmpty()) addr = "224.0.0.1";
        auto& s = AppSettings::instance();
        s.setValue("WsjtxAddress", addr);
        s.setValue("WsjtxPort", port);
        s.save();
        emit wsjtxStartRequested(addr, port);
    });
    btnRow->addWidget(m_wsjtxStartBtn);
    connLayout->addLayout(btnRow);

    layout->addWidget(connGroup);

    // ── Console output ──────────────────────────────────────────────────
    // ── Spot filters with color pickers ────────────────────────────────
    auto* filterRow = new QHBoxLayout;
    filterRow->setSpacing(6);
    auto* filterLabel = new QLabel("Spot Filter:");
    filterLabel->setStyleSheet("QLabel { color: #808080; font-size: 14px; }");
    filterRow->addWidget(filterLabel);

    QString cbStyle = "QCheckBox { color: #a0b0c0; font-size: 14px; spacing: 3px; }"
                      "QCheckBox::indicator { width: 14px; height: 14px; }";
    auto swatchStyle = [](const QColor& c) {
        return QString("QPushButton { background: %1; border: 2px solid #405060; border-radius: 3px; }"
                       "QPushButton:hover { border-color: #c8d8e8; }").arg(c.name());
    };

    // CQ color + checkbox
    QColor cqColor(s.value("WsjtxColorCQ", "#00FF00").toString());
    m_wsjtxColorCQ = new QPushButton;
    m_wsjtxColorCQ->setFixedSize(18, 18);
    m_wsjtxColorCQ->setStyleSheet(swatchStyle(cqColor));
    connect(m_wsjtxColorCQ, &QPushButton::clicked, this, [this, swatchStyle] {
        QColor c = QColorDialog::getColor(QColor(AppSettings::instance().value("WsjtxColorCQ", "#00FF00").toString()), this, "CQ Spot Color");
        if (c.isValid()) {
            m_wsjtxColorCQ->setStyleSheet(swatchStyle(c));
            AppSettings::instance().setValue("WsjtxColorCQ", c.name());
            AppSettings::instance().save();
        }
    });
    filterRow->addWidget(m_wsjtxColorCQ);

    m_wsjtxFilterCQ = new QCheckBox("CQ");
    m_wsjtxFilterCQ->setChecked(s.value("WsjtxFilterCQ", "True").toString() == "True");
    m_wsjtxFilterCQ->setStyleSheet(cbStyle);
    connect(m_wsjtxFilterCQ, &QCheckBox::toggled, this, [this](bool on) {
        if (on) m_wsjtxFilterPOTA->setChecked(false);
        auto& s = AppSettings::instance();
        s.setValue("WsjtxFilterCQ", on ? "True" : "False");
        s.save();
    });
    filterRow->addWidget(m_wsjtxFilterCQ, 1);

    // CQ POTA color + checkbox
    QColor potaColor(s.value("WsjtxColorPOTA", "#00FFFF").toString());
    m_wsjtxColorPOTA = new QPushButton;
    m_wsjtxColorPOTA->setFixedSize(18, 18);
    m_wsjtxColorPOTA->setStyleSheet(swatchStyle(potaColor));
    connect(m_wsjtxColorPOTA, &QPushButton::clicked, this, [this, swatchStyle] {
        QColor c = QColorDialog::getColor(QColor(AppSettings::instance().value("WsjtxColorPOTA", "#00FFFF").toString()), this, "CQ POTA Spot Color");
        if (c.isValid()) {
            m_wsjtxColorPOTA->setStyleSheet(swatchStyle(c));
            AppSettings::instance().setValue("WsjtxColorPOTA", c.name());
            AppSettings::instance().save();
        }
    });
    filterRow->addWidget(m_wsjtxColorPOTA);

    m_wsjtxFilterPOTA = new QCheckBox("CQ POTA");
    m_wsjtxFilterPOTA->setChecked(s.value("WsjtxFilterPOTA", "True").toString() == "True");
    m_wsjtxFilterPOTA->setStyleSheet(cbStyle);
    connect(m_wsjtxFilterPOTA, &QCheckBox::toggled, this, [this](bool on) {
        if (on) m_wsjtxFilterCQ->setChecked(false);
        auto& s = AppSettings::instance();
        s.setValue("WsjtxFilterPOTA", on ? "True" : "False");
        s.save();
    });
    filterRow->addWidget(m_wsjtxFilterPOTA, 1);

    // Calling Me color + checkbox
    QColor callingMeColor(s.value("WsjtxColorCallingMe", "#FF0000").toString());
    m_wsjtxColorCallingMe = new QPushButton;
    m_wsjtxColorCallingMe->setFixedSize(18, 18);
    m_wsjtxColorCallingMe->setStyleSheet(swatchStyle(callingMeColor));
    connect(m_wsjtxColorCallingMe, &QPushButton::clicked, this, [this, swatchStyle] {
        QColor c = QColorDialog::getColor(QColor(AppSettings::instance().value("WsjtxColorCallingMe", "#FF0000").toString()), this, "Calling Me Spot Color");
        if (c.isValid()) {
            m_wsjtxColorCallingMe->setStyleSheet(swatchStyle(c));
            AppSettings::instance().setValue("WsjtxColorCallingMe", c.name());
            AppSettings::instance().save();
        }
    });
    filterRow->addWidget(m_wsjtxColorCallingMe);

    m_wsjtxFilterCallingMe = new QCheckBox("Calling Me");
    m_wsjtxFilterCallingMe->setChecked(s.value("WsjtxFilterCallingMe", "True").toString() == "True");
    m_wsjtxFilterCallingMe->setStyleSheet(cbStyle);
    connect(m_wsjtxFilterCallingMe, &QCheckBox::toggled, this, [](bool on) {
        auto& s = AppSettings::instance();
        s.setValue("WsjtxFilterCallingMe", on ? "True" : "False");
        s.save();
    });
    filterRow->addWidget(m_wsjtxFilterCallingMe, 1);

    // Default color (no checkbox)
    QColor defaultColor(s.value("WsjtxColorDefault", "#FFFFFF").toString());
    m_wsjtxColorDefault = new QPushButton;
    m_wsjtxColorDefault->setFixedSize(18, 18);
    m_wsjtxColorDefault->setStyleSheet(swatchStyle(defaultColor));
    connect(m_wsjtxColorDefault, &QPushButton::clicked, this, [this, swatchStyle] {
        QColor c = QColorDialog::getColor(QColor(AppSettings::instance().value("WsjtxColorDefault", "#FFFFFF").toString()), this, "Default Spot Color");
        if (c.isValid()) {
            m_wsjtxColorDefault->setStyleSheet(swatchStyle(c));
            AppSettings::instance().setValue("WsjtxColorDefault", c.name());
            AppSettings::instance().save();
        }
    });
    filterRow->addWidget(m_wsjtxColorDefault);
    auto* defaultLabel = new QLabel("Default");
    defaultLabel->setStyleSheet("QLabel { color: #a0b0c0; font-size: 14px; }");
    filterRow->addWidget(defaultLabel);

    layout->addLayout(filterRow);

    // ── Console output ──────────────────────────────────────────────────
    // Decodes label + spot lifetime slider
    auto* decodeRow = new QHBoxLayout;
    auto* consoleLabel = new QLabel("WSJT-X Decodes");
    consoleLabel->setStyleSheet("QLabel { color: #00b4d8; font-weight: bold; }");
    decodeRow->addWidget(consoleLabel);
    decodeRow->addStretch();

    auto* lifeLabel = new QLabel("Spot Life:");
    lifeLabel->setStyleSheet("QLabel { color: #808080; font-size: 12px; }");
    decodeRow->addWidget(lifeLabel);

    int wsjtxLife = s.value("WsjtxSpotLifetime", 120).toInt();
    auto* wsjtxLifeSlider = new GuardedSlider(Qt::Horizontal);
    wsjtxLifeSlider->setRange(30, 300);
    wsjtxLifeSlider->setValue(wsjtxLife);
    wsjtxLifeSlider->setFixedWidth(120);
    decodeRow->addWidget(wsjtxLifeSlider);

    auto* wsjtxLifeValue = new QLabel(QString("%1s").arg(wsjtxLife));
    wsjtxLifeValue->setFixedWidth(35);
    wsjtxLifeValue->setAlignment(Qt::AlignRight);
    wsjtxLifeValue->setStyleSheet("QLabel { color: #a0b0c0; font-size: 12px; }");
    decodeRow->addWidget(wsjtxLifeValue);

    connect(wsjtxLifeSlider, &QSlider::valueChanged, this, [wsjtxLifeValue](int v) {
        wsjtxLifeValue->setText(QString("%1s").arg(v));
        auto& s = AppSettings::instance();
        s.setValue("WsjtxSpotLifetime", v);
        s.save();
    });
    layout->addLayout(decodeRow);

    m_wsjtxConsole = new QPlainTextEdit;
    m_wsjtxConsole->setReadOnly(true);
    m_wsjtxConsole->setMaximumBlockCount(2000);
    m_wsjtxConsole->setStyleSheet(
        "QPlainTextEdit {"
        "  background: #0a0a14;"
        "  color: #a0b0c0;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "  border: 1px solid #203040;"
        "  padding: 4px;"
        "}");
    layout->addWidget(m_wsjtxConsole, 1);

    tabs->addTab(page, "WSJT-X");
}

void DxClusterDialog::buildSpotCollectorTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setSpacing(8);

    auto& s = AppSettings::instance();

    // ── Connection settings ─────────────────────────────────────────────
    auto* connGroup = new QGroupBox("SpotCollector UDP Listener");
    auto* connLayout = new QVBoxLayout(connGroup);
    connLayout->setSpacing(4);

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);

    grid->addWidget(new QLabel("UDP Port:"), 0, 0);
    m_scPortSpin = new QSpinBox;
    m_scPortSpin->setRange(1, 65535);
    m_scPortSpin->setValue(s.value("SpotCollectorPort", 9999).toInt());
    m_scPortSpin->setStyleSheet("QSpinBox { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    grid->addWidget(m_scPortSpin, 0, 1);

    connLayout->addLayout(grid);

    auto* helpLabel = new QLabel(
        "Receives DX spots from DXLab SpotCollector via UDP push.\n"
        "In SpotCollector, enable UDP broadcast to this port (default 9999).\n"
        "Alternatively, use the DX Cluster tab to connect to SpotCollector's telnet interface.");
    helpLabel->setWordWrap(true);
    helpLabel->setStyleSheet("QLabel { color: #8aa8c0; font-size: 11px; }");
    connLayout->addWidget(helpLabel);

    // Button row
    auto* btnRow = new QHBoxLayout;
    m_scAutoStartBtn = new QPushButton(
        s.value("SpotCollectorAutoStart", "False").toString() == "True" ? "Auto-Start: ON" : "Auto-Start: OFF");
    m_scAutoStartBtn->setCheckable(true);
    m_scAutoStartBtn->setChecked(s.value("SpotCollectorAutoStart", "False").toString() == "True");
    m_scAutoStartBtn->setStyleSheet(
        kSpotHubToggle);
    connect(m_scAutoStartBtn, &QPushButton::toggled, this, [this](bool on) {
        m_scAutoStartBtn->setText(on ? "Auto-Start: ON" : "Auto-Start: OFF");
        auto& s = AppSettings::instance();
        s.setValue("SpotCollectorAutoStart", on ? "True" : "False");
        s.save();
    });
    btnRow->addWidget(m_scAutoStartBtn);
    btnRow->addStretch();

    m_scStatusLabel = new QLabel("Stopped");
    m_scStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
    btnRow->addWidget(m_scStatusLabel);
    btnRow->addStretch();

    m_scStartBtn = new QPushButton(m_spotCollectorClient->isListening() ? "Stop" : "Start");
    m_scStartBtn->setFixedWidth(100);
    m_scStartBtn->setStyleSheet(
        "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold; "
        "border: 1px solid #008ba8; padding: 4px; border-radius: 3px; }"
        "QPushButton:hover { background: #00c8f0; }"
        "QPushButton:disabled { background: #404060; color: #808080; }");
    connect(m_scStartBtn, &QPushButton::clicked, this, [this] {
        if (m_spotCollectorClient->isListening()) {
            emit spotCollectorStopRequested();
            return;
        }
        quint16 port = static_cast<quint16>(m_scPortSpin->value());
        auto& s = AppSettings::instance();
        s.setValue("SpotCollectorPort", port);
        s.save();
        emit spotCollectorStartRequested(port);
    });
    btnRow->addWidget(m_scStartBtn);
    connLayout->addLayout(btnRow);

    layout->addWidget(connGroup);

    // ── Console output ──────────────────────────────────────────────────
    auto* consoleLabel = new QLabel("SpotCollector Spots");
    consoleLabel->setStyleSheet("QLabel { color: #00b4d8; font-weight: bold; }");
    layout->addWidget(consoleLabel);

    m_scConsole = new QPlainTextEdit;
    m_scConsole->setReadOnly(true);
    m_scConsole->setMaximumBlockCount(2000);
    m_scConsole->setStyleSheet(
        "QPlainTextEdit {"
        "  background: #0a0a14;"
        "  color: #a0b0c0;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "  border: 1px solid #203040;"
        "  padding: 4px;"
        "}");
    layout->addWidget(m_scConsole, 1);

    // Update status if already listening
    if (m_spotCollectorClient->isListening()) {
        m_scStatusLabel->setText(QString("Listening on port %1").arg(m_scPortSpin->value()));
        m_scStatusLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 11px; }");
        m_scStartBtn->setText("Stop");
    }

    tabs->addTab(page, "SpotCollector");
}

void DxClusterDialog::buildPotaTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setSpacing(8);

    auto& s = AppSettings::instance();

    // ── Settings ────────────────────────────────────────────────────────
    auto* connGroup = new QGroupBox("POTA Spot Feed");
    auto* connLayout = new QVBoxLayout(connGroup);
    connLayout->setSpacing(4);

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    int row = 0;

    grid->addWidget(new QLabel("Server:"), row, 0);
    auto* serverLabel = new QLabel("api.pota.app (HTTP polling)");
    serverLabel->setStyleSheet("QLabel { color: #808890; }");
    grid->addWidget(serverLabel, row, 1);
    row++;

    grid->addWidget(new QLabel("Poll Interval:"), row, 0);
    m_potaIntervalSpin = new QSpinBox;
    m_potaIntervalSpin->setRange(15, 300);
    m_potaIntervalSpin->setValue(s.value("PotaPollInterval", 30).toInt());
    m_potaIntervalSpin->setSuffix(" sec");
    m_potaIntervalSpin->setStyleSheet("QSpinBox { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    connect(m_potaIntervalSpin, &QSpinBox::valueChanged, this, [](int v) {
        auto& s = AppSettings::instance();
        s.setValue("PotaPollInterval", v);
        s.save();
    });
    grid->addWidget(m_potaIntervalSpin, row, 1);
    row++;

    connLayout->addLayout(grid);

    // Button row
    auto* btnRow = new QHBoxLayout;
    m_potaAutoStartBtn = new QPushButton(
        s.value("PotaAutoStart", "False").toString() == "True" ? "Auto-Start: ON" : "Auto-Start: OFF");
    m_potaAutoStartBtn->setCheckable(true);
    m_potaAutoStartBtn->setChecked(s.value("PotaAutoStart", "False").toString() == "True");
    m_potaAutoStartBtn->setStyleSheet(
        kSpotHubToggle);
    connect(m_potaAutoStartBtn, &QPushButton::toggled, this, [this](bool on) {
        m_potaAutoStartBtn->setText(on ? "Auto-Start: ON" : "Auto-Start: OFF");
        auto& s = AppSettings::instance();
        s.setValue("PotaAutoStart", on ? "True" : "False");
        s.save();
    });
    btnRow->addWidget(m_potaAutoStartBtn);
    btnRow->addStretch();

    m_potaStatusLabel = new QLabel("Stopped");
    m_potaStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
    btnRow->addWidget(m_potaStatusLabel);
    btnRow->addStretch();

    m_potaStartBtn = new QPushButton(m_potaClient->isPolling() ? "Stop" : "Start");
    m_potaStartBtn->setFixedWidth(100);
    m_potaStartBtn->setStyleSheet(
        "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold; "
        "border: 1px solid #008ba8; padding: 4px; border-radius: 3px; }"
        "QPushButton:hover { background: #00c8f0; }"
        "QPushButton:disabled { background: #404060; color: #808080; }");
    connect(m_potaStartBtn, &QPushButton::clicked, this, [this] {
        if (m_potaClient->isPolling()) {
            emit potaStopRequested();
            return;
        }
        int interval = m_potaIntervalSpin->value();
        auto& s = AppSettings::instance();
        s.setValue("PotaPollInterval", interval);
        s.save();
        emit potaStartRequested(interval);
    });
    btnRow->addWidget(m_potaStartBtn);
    connLayout->addLayout(btnRow);

    layout->addWidget(connGroup);

    // ── Console output ──────────────────────────────────────────────────
    auto* consoleRow = new QHBoxLayout;
    auto* consoleLabel = new QLabel("POTA Activations");
    consoleLabel->setStyleSheet("QLabel { color: #00b4d8; font-weight: bold; }");
    consoleRow->addWidget(consoleLabel);
    consoleRow->addStretch();

    auto* spotColorLabel = new QLabel("Spot Color:");
    spotColorLabel->setStyleSheet("QLabel { color: #808080; font-size: 12px; }");
    consoleRow->addWidget(spotColorLabel);

    QColor potaColor(s.value("PotaSpotColor", "#FFFF00").toString());
    auto* potaColorBtn = new QPushButton;
    potaColorBtn->setFixedSize(18, 18);
    potaColorBtn->setStyleSheet(QString(
        "QPushButton { background: %1; border: 2px solid #405060; border-radius: 3px; }"
        "QPushButton:hover { border-color: #c8d8e8; }").arg(potaColor.name()));
    connect(potaColorBtn, &QPushButton::clicked, this, [this, potaColorBtn] {
        QColor c = QColorDialog::getColor(
            QColor(AppSettings::instance().value("PotaSpotColor", "#FFFF00").toString()),
            this, "POTA Spot Color");
        if (c.isValid()) {
            potaColorBtn->setStyleSheet(QString(
                "QPushButton { background: %1; border: 2px solid #405060; border-radius: 3px; }"
                "QPushButton:hover { border-color: #c8d8e8; }").arg(c.name()));
            AppSettings::instance().setValue("PotaSpotColor", c.name());
            AppSettings::instance().save();
        }
    });
    consoleRow->addWidget(potaColorBtn);
    layout->addLayout(consoleRow);

    m_potaConsole = new QPlainTextEdit;
    m_potaConsole->setReadOnly(true);
    m_potaConsole->setMaximumBlockCount(2000);
    m_potaConsole->setStyleSheet(
        "QPlainTextEdit {"
        "  background: #0a0a14;"
        "  color: #a0b0c0;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "  border: 1px solid #203040;"
        "  padding: 4px;"
        "}");
    layout->addWidget(m_potaConsole, 1);

    tabs->addTab(page, "POTA");
}

#ifdef HAVE_WEBSOCKETS
void DxClusterDialog::buildFreeDvTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setSpacing(8);

    auto& s = AppSettings::instance();

    // ── Settings ────────────────────────────────────────────────────────
    auto* connGroup = new QGroupBox("FreeDV QSO Reporter");
    auto* connLayout = new QVBoxLayout(connGroup);
    connLayout->setSpacing(4);

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    int row = 0;

    grid->addWidget(new QLabel("Server:"), row, 0);
    auto* serverLabel = new QLabel("qso.freedv.org (WebSocket)");
    serverLabel->setStyleSheet("QLabel { color: #808890; }");
    grid->addWidget(serverLabel, row, 1);
    row++;

    connLayout->addLayout(grid);

    // Button row
    auto* btnRow = new QHBoxLayout;
    m_freedvAutoStartBtn = new QPushButton(
        s.value("FreeDvAutoStart", "False").toString() == "True" ? "Auto-Start: ON" : "Auto-Start: OFF");
    m_freedvAutoStartBtn->setCheckable(true);
    m_freedvAutoStartBtn->setChecked(s.value("FreeDvAutoStart", "False").toString() == "True");
    m_freedvAutoStartBtn->setStyleSheet(
        kSpotHubToggle);
    connect(m_freedvAutoStartBtn, &QPushButton::toggled, this, [this](bool on) {
        m_freedvAutoStartBtn->setText(on ? "Auto-Start: ON" : "Auto-Start: OFF");
        auto& s = AppSettings::instance();
        s.setValue("FreeDvAutoStart", on ? "True" : "False");
        s.save();
    });
    btnRow->addWidget(m_freedvAutoStartBtn);
    btnRow->addStretch();

    m_freedvStatusLabel = new QLabel("Stopped");
    m_freedvStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
    btnRow->addWidget(m_freedvStatusLabel);
    btnRow->addStretch();

    m_freedvStartBtn = new QPushButton(m_freedvClient->isConnected() ? "Stop" : "Start");
    m_freedvStartBtn->setFixedWidth(100);
    m_freedvStartBtn->setStyleSheet(
        "QPushButton { background: #00b4d8; color: #0f0f1a; font-weight: bold; "
        "border: 1px solid #008ba8; padding: 4px; border-radius: 3px; }"
        "QPushButton:hover { background: #00c8f0; }"
        "QPushButton:disabled { background: #404060; color: #808080; }");
    connect(m_freedvStartBtn, &QPushButton::clicked, this, [this] {
        if (m_freedvClient->isConnected()) {
            emit freedvStopRequested();
            return;
        }
        emit freedvStartRequested();
    });
    btnRow->addWidget(m_freedvStartBtn);
    connLayout->addLayout(btnRow);

    layout->addWidget(connGroup);

    // ── Station Reporting ────────────────────────────────────────────────
    auto* reportGroup = new QGroupBox("Station Reporting");
    auto* reportLayout = new QGridLayout(reportGroup);
    reportLayout->setSpacing(6);
    reportLayout->setColumnStretch(1, 1);
    int frow = 0;

    const QString fdvCheckStyle =
        "QCheckBox { color: #d7e4f2; spacing: 8px; background: transparent; border: none; }"
        "QCheckBox::indicator { width: 14px; height: 14px; border: 2px solid #5d748d; border-radius: 3px; background: #0b1520; }"
        "QCheckBox::indicator:hover { border-color: #81abd9; background: #142130; }"
        "QCheckBox::indicator:checked { border: 2px solid #8cc8ff; background: #2f71b6; }"
        "QCheckBox::indicator:disabled { border-color: #405262; background: #10161d; }";

    // Enable checkbox spans all columns so it sits reliably inside the grid,
    // not above it (avoids group-box title margin clipping on dark themes).
    m_fdvReportCheck = new QCheckBox("Enable FreeDV Reporter reporting when RADE is active");
    m_fdvReportCheck->setChecked(s.value("FreeDvAutoReport", "False").toString() == "True");
    m_fdvReportCheck->setStyleSheet(fdvCheckStyle);
    connect(m_fdvReportCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (on) {
            // Resolve the effective callsign and grid the same way
            // MainWindow::startFreeDvReporting() does — radio/GPS first,
            // user-entered fields as fallback.  Refuse to enable if either
            // is empty so we don't broadcast placeholder data ("N0CALL" /
            // "AA00") to the public FreeDV Reporter map.
            QString callsign;
            if (m_fdvUseRadioCallsignCheck->isChecked()
                    && !m_radioModel->callsign().isEmpty()) {
                callsign = m_radioModel->callsign();
            } else {
                callsign = m_fdvCallsignEdit->text().trimmed();
            }

            QString grid;
            if (m_fdvUseGpsCheck && m_fdvUseGpsCheck->isChecked()
                    && m_radioModel->hasGpsHardware()
                    && !m_radioModel->gpsGrid().isEmpty()) {
                grid = m_radioModel->gpsGrid();
            } else {
                grid = m_fdvGridEdit->text().trimmed();
            }

            if (callsign.isEmpty() || grid.isEmpty()) {
                QMessageBox::warning(this, "FreeDV Reporter",
                    "Please set both a callsign and a grid square before "
                    "enabling reporter broadcasting.\n\n"
                    "Reporter broadcasts to a public, community-shared map; "
                    "blank or placeholder values would pollute it.");
                QSignalBlocker block(m_fdvReportCheck);
                m_fdvReportCheck->setChecked(false);
                return;
            }
        }
        auto& as = AppSettings::instance();
        as.setValue("FreeDvAutoReport", on ? "True" : "False");
        as.save();
        emit freedvReportingToggled(on);
    });
    reportLayout->addWidget(m_fdvReportCheck, frow, 0, 1, 3);
    frow++;

    // Callsign row — all radio models have a callsign field
    reportLayout->addWidget(new QLabel("Callsign:"), frow, 0);
    m_fdvCallsignEdit = new QLineEdit;
    m_fdvCallsignEdit->setPlaceholderText("N0CALL");
    m_fdvCallsignEdit->setMaxLength(10);
    {
        bool useRadio = s.value("FreeDvUseRadioCallsign", "True").toString() == "True";
        const QString radioCall = m_radioModel->callsign();
        m_fdvCallsignEdit->setText(
            useRadio && !radioCall.isEmpty() ? radioCall
                                             : s.value("FreeDvMyCallsign", "").toString());
        m_fdvCallsignEdit->setReadOnly(useRadio);
    }
    m_fdvCallsignEdit->setStyleSheet(
        "QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }"
        "QLineEdit[readOnly=\"true\"] { color: #607080; }");
    connect(m_fdvCallsignEdit, &QLineEdit::editingFinished, this, [this] {
        auto& as = AppSettings::instance();
        as.setValue("FreeDvMyCallsign", m_fdvCallsignEdit->text().trimmed().toUpper());
        as.save();
    });
    reportLayout->addWidget(m_fdvCallsignEdit, frow, 1);

    m_fdvUseRadioCallsignCheck = new QCheckBox("Use radio");
    m_fdvUseRadioCallsignCheck->setChecked(
        s.value("FreeDvUseRadioCallsign", "True").toString() == "True");
    m_fdvUseRadioCallsignCheck->setStyleSheet(fdvCheckStyle);
    connect(m_fdvUseRadioCallsignCheck, &QCheckBox::toggled, this, [this](bool on) {
        auto& as = AppSettings::instance();
        as.setValue("FreeDvUseRadioCallsign", on ? "True" : "False");
        as.save();
        m_fdvCallsignEdit->setReadOnly(on);
        if (on && !m_radioModel->callsign().isEmpty())
            m_fdvCallsignEdit->setText(m_radioModel->callsign());
    });
    // Sync when the user changes the callsign in Radio Setup
    connect(m_radioModel, &RadioModel::infoChanged, this, [this] {
        if (m_fdvUseRadioCallsignCheck->isChecked() && !m_radioModel->callsign().isEmpty())
            m_fdvCallsignEdit->setText(m_radioModel->callsign());
    });
    reportLayout->addWidget(m_fdvUseRadioCallsignCheck, frow, 2);
    frow++;

    // Grid Square row
    reportLayout->addWidget(new QLabel("Grid Square:"), frow, 0);
    m_fdvGridEdit = new QLineEdit;
    m_fdvGridEdit->setPlaceholderText("AA00");
    m_fdvGridEdit->setMaxLength(6);
    m_fdvGridEdit->setText(s.value("FreeDvMyGrid", "").toString());
    m_fdvGridEdit->setStyleSheet(
        "QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }"
        "QLineEdit[readOnly=\"true\"] { color: #607080; }");
    connect(m_fdvGridEdit, &QLineEdit::editingFinished, this, [this] {
        auto& as = AppSettings::instance();
        as.setValue("FreeDvMyGrid", m_fdvGridEdit->text().trimmed().toUpper());
        as.save();
    });
    reportLayout->addWidget(m_fdvGridEdit, frow, 1);

    // GPS checkbox — only on FLEX-8000 class and Aurora, which have GPS hardware
    if (m_radioModel->hasGpsHardware()) {
        m_fdvUseGpsCheck = new QCheckBox("Use GPS");
        m_fdvUseGpsCheck->setStyleSheet(fdvCheckStyle);
        bool useGps = s.value("FreeDvUseGpsGrid", "True").toString() == "True";
        m_fdvUseGpsCheck->setChecked(useGps);
        m_fdvGridEdit->setReadOnly(useGps);
        if (useGps && !m_radioModel->gpsGrid().isEmpty())
            m_fdvGridEdit->setText(m_radioModel->gpsGrid());
        connect(m_fdvUseGpsCheck, &QCheckBox::toggled, this, [this](bool on) {
            auto& as = AppSettings::instance();
            as.setValue("FreeDvUseGpsGrid", on ? "True" : "False");
            as.save();
            m_fdvGridEdit->setReadOnly(on);
            if (on && !m_radioModel->gpsGrid().isEmpty())
                m_fdvGridEdit->setText(m_radioModel->gpsGrid());
        });
        // Auto-update when GPS acquires or re-acquires lock
        connect(m_radioModel, &RadioModel::gpsStatusChanged, this,
                [this](const QString&, int, int, const QString& gpsGrid,
                       const QString&, const QString&, const QString&, const QString&) {
                    if (m_fdvUseGpsCheck->isChecked() && !gpsGrid.isEmpty())
                        m_fdvGridEdit->setText(gpsGrid);
                });
        reportLayout->addWidget(m_fdvUseGpsCheck, frow, 2);
    }
    frow++;

    // Station Message row
    reportLayout->addWidget(new QLabel("Station Msg:"), frow, 0);
    m_fdvMessageEdit = new QLineEdit;
    m_fdvMessageEdit->setPlaceholderText("Optional message shown on reporter map");
    m_fdvMessageEdit->setText(s.value("FreeDvMyMessage", "").toString());
    m_fdvMessageEdit->setStyleSheet(
        "QLineEdit { background: #1a1a2e; color: #c8d8e8; border: 1px solid #203040; padding: 3px; }");
    connect(m_fdvMessageEdit, &QLineEdit::editingFinished, this, [this] {
        auto& as = AppSettings::instance();
        QString msg = m_fdvMessageEdit->text().trimmed();
        as.setValue("FreeDvMyMessage", msg);
        as.save();
        emit freedvMessageChanged(msg);
    });
    reportLayout->addWidget(m_fdvMessageEdit, frow, 1);
    frow++;

    layout->addWidget(reportGroup);

    // ── Console output ──────────────────────────────────────────────────
    auto* consoleRow = new QHBoxLayout;
    auto* consoleLabel = new QLabel("FreeDV Spots");
    consoleLabel->setStyleSheet("QLabel { color: #00b4d8; font-weight: bold; }");
    consoleRow->addWidget(consoleLabel);
    consoleRow->addStretch();

    auto* spotColorLabel = new QLabel("Spot Color:");
    spotColorLabel->setStyleSheet("QLabel { color: #808080; font-size: 12px; }");
    consoleRow->addWidget(spotColorLabel);

    QColor freedvColor(s.value("FreeDvSpotColor", "#FF8C00").toString());
    auto* freedvColorBtn = new QPushButton;
    freedvColorBtn->setFixedSize(18, 18);
    freedvColorBtn->setStyleSheet(QString(
        "QPushButton { background: %1; border: 2px solid #405060; border-radius: 3px; }"
        "QPushButton:hover { border-color: #c8d8e8; }").arg(freedvColor.name()));
    connect(freedvColorBtn, &QPushButton::clicked, this, [this, freedvColorBtn] {
        QColor c = QColorDialog::getColor(
            QColor(AppSettings::instance().value("FreeDvSpotColor", "#FF8C00").toString()),
            this, "FreeDV Spot Color");
        if (c.isValid()) {
            freedvColorBtn->setStyleSheet(QString(
                "QPushButton { background: %1; border: 2px solid #405060; border-radius: 3px; }"
                "QPushButton:hover { border-color: #c8d8e8; }").arg(c.name()));
            AppSettings::instance().setValue("FreeDvSpotColor", c.name());
            AppSettings::instance().save();
        }
    });
    consoleRow->addWidget(freedvColorBtn);
    layout->addLayout(consoleRow);

    m_freedvConsole = new QPlainTextEdit;
    m_freedvConsole->setReadOnly(true);
    m_freedvConsole->setMaximumBlockCount(2000);
    m_freedvConsole->setStyleSheet(
        "QPlainTextEdit {"
        "  background: #0a0a14;"
        "  color: #a0b0c0;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "  border: 1px solid #203040;"
        "  padding: 4px;"
        "}");
    layout->addWidget(m_freedvConsole, 1);

    tabs->addTab(page, "FreeDV");
}
#endif

void DxClusterDialog::buildSpotListTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setSpacing(4);

    // Band filter checkboxes
    auto* filterRow = new QHBoxLayout;
    filterRow->setSpacing(2);
    auto* filterLabel = new QLabel("Bands:");
    filterLabel->setStyleSheet("QLabel { color: #808080; font-size: 13px; }");
    filterRow->addWidget(filterLabel);

    // Table model + band filter proxy
    m_spotModel = new SpotTableModel(this);
    m_proxyModel = new BandFilterProxy(this);
    m_proxyModel->setSourceModel(m_spotModel);
    m_proxyModel->setSortRole(Qt::UserRole);

    static constexpr const char* bands[] = {
        "160m", "80m", "60m", "40m", "30m", "20m", "17m", "15m", "12m", "10m", "6m"
    };
    QString cbStyle =
        "QCheckBox { color: #a0b0c0; font-size: 12px; spacing: 3px; }"
        "QCheckBox::indicator { width: 13px; height: 13px; }";
    auto& sf = AppSettings::instance();
    for (const char* band : bands) {
        auto* cb = new QCheckBox(band);
        QString key = QString("SpotBandFilter_%1").arg(band);
        bool on = sf.value(key, "True").toString() == "True";
        cb->setChecked(on);
        if (!on)
            m_proxyModel->setBandVisible(QString(band), false);
        cb->setStyleSheet(cbStyle);
        connect(cb, &QCheckBox::toggled, this, [this, b = QString(band), key](bool on) {
            m_proxyModel->setBandVisible(b, on);
            auto& s = AppSettings::instance();
            s.setValue(key, on ? "True" : "False");
            s.save();
        });
        filterRow->addWidget(cb, 1);  // equal stretch across row
    }
    layout->addLayout(filterRow);

    m_spotTable = new QTableView;
    m_spotTable->setModel(m_proxyModel);
    m_spotTable->setSortingEnabled(true);
    m_spotTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_spotTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_spotTable->setAlternatingRowColors(true);
    m_spotTable->verticalHeader()->setVisible(false);
    m_spotTable->verticalHeader()->setDefaultSectionSize(20);
    m_spotTable->horizontalHeader()->setStretchLastSection(true);
    m_spotTable->setStyleSheet(
        "QTableView {"
        "  background: #0a0a14;"
        "  alternate-background-color: #0f0f1e;"
        "  color: #c8d8e8;"
        "  gridline-color: #1a2a3a;"
        "  border: 1px solid #203040;"
        "  font-size: 11px;"
        "}"
        "QTableView::item:selected {"
        "  background: #1a3a5a;"
        "  color: #e0f0ff;"
        "}"
        "QHeaderView::section {"
        "  background: #1a1a2e;"
        "  color: #00b4d8;"
        "  border: 1px solid #203040;"
        "  padding: 3px 6px;"
        "  font-weight: bold;"
        "  font-size: 11px;"
        "}");

    // Column widths
    m_spotTable->setColumnWidth(SpotTableModel::ColTime, 50);
    m_spotTable->setColumnWidth(SpotTableModel::ColFreq, 80);
    m_spotTable->setColumnWidth(SpotTableModel::ColDxCall, 90);
    m_spotTable->setColumnWidth(SpotTableModel::ColMode, 45);
    m_spotTable->setColumnWidth(SpotTableModel::ColComment, 200);
    m_spotTable->setColumnWidth(SpotTableModel::ColSpotter, 80);
    m_spotTable->setColumnWidth(SpotTableModel::ColBand, 45);
    m_spotTable->setColumnWidth(SpotTableModel::ColSource, 55);

    // No default sort — insertion order is newest-first
    m_spotTable->horizontalHeader()->setSortIndicatorShown(false);

    // Double-click to tune (#2298: also forward mode hints so the receiver
    // switches CW/SSB to match the spot rather than only changing frequency).
    connect(m_spotTable, &QTableView::doubleClicked, this, [this](const QModelIndex& idx) {
        auto srcIdx = m_proxyModel->mapToSource(idx);
        const DxSpot* spot = m_spotModel->spotAt(srcIdx.row());
        if (!spot || spot->freqMhz <= 0.0) return;
        emit tuneRequested(spot->freqMhz,
                           SpotTableModel::extractMode(spot->comment),
                           spot->comment);
    });

    layout->addWidget(m_spotTable, 1);

    // Bottom bar: spot count + clear
    auto* bottomRow = new QHBoxLayout;
    auto* countLabel = new QLabel("0 spots");
    countLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
    connect(m_spotModel, &QAbstractTableModel::rowsInserted, this, [this, countLabel] {
        countLabel->setText(QString("%1 spots").arg(m_spotModel->rowCount()));
    });
    bottomRow->addWidget(countLabel);
    bottomRow->addStretch();

    auto* clearBtn = new QPushButton("Clear");
    clearBtn->setFixedWidth(60);
    connect(clearBtn, &QPushButton::clicked, this, [this, countLabel] {
        m_spotModel->clear();
        countLabel->setText("0 spots");
    });
    bottomRow->addWidget(clearBtn);
    layout->addLayout(bottomRow);

    tabs->addTab(page, "Spot List");
}

void DxClusterDialog::buildDisplayTab(QTabWidget* tabs)
{
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setSpacing(8);

    auto& s = AppSettings::instance();
    bool spotsEnabled     = s.value("IsSpotsEnabled", "True").toString() == "True";
    bool passiveSpots     = SpotCommandPolicy::passiveModeFromSetting(
                                s.value(SpotCommandPolicy::kPassiveSpotsModeKey, "False"));
    bool memoriesEnabled  = s.value("IsMemorySpotsEnabled", "False").toString() == "True";
    bool autoMode         = s.value("SpotAutoSwitchMode", "True").toString() == "True";
    bool sHistorySignals  = s.value("SHistoryMarkersEnabled", "False").toString() == "True";
    bool sHistoryQrm      = s.value("SHistoryQrmEnabled", "False").toString() == "True";
    bool overrideColors   = s.value("IsSpotsOverrideColorsEnabled", "False").toString() == "True";
    bool overrideBg       = s.value("IsSpotsOverrideBackgroundColorsEnabled", "True").toString() == "True";
    bool overrideBgAuto   = s.value("IsSpotsOverrideToAutoBackgroundColorEnabled", "True").toString() == "True";
    bool spotLines        = s.value("IsSpotsLinesEnabled", "True").toString() == "True";
    int levels            = s.value("SpotsMaxLevel", 3).toInt();
    int position          = s.value("SpotsStartingHeightPercentage", 50).toInt();
    int fontSize          = s.value("SpotFontSize", 16).toInt();
    int lifetimeSec       = s.value("DxClusterSpotLifetimeSec", 0).toInt();
    if (lifetimeSec <= 0)
        lifetimeSec = s.value("DxClusterSpotLifetime", 30).toInt() * 60;
    QColor spotColor(s.value("SpotsOverrideColor", "#FFFF00").toString());
    QColor bgColor(s.value("SpotsOverrideBgColor", "#000000").toString());
    int bgOpacity         = s.value("SpotsBackgroundOpacity", 48).toInt();

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    int row = 0;

    auto save = [this](const QString& key, const QVariant& val) {
        auto& s = AppSettings::instance();
        s.setValue(key, val);
        s.save();
        emit settingsChanged();
    };

    auto updateSwatch = [](QPushButton* btn, const QColor& color) {
        btn->setStyleSheet(QString(
            "QPushButton { background: %1; border: 2px solid #405060; border-radius: 3px; }"
            "QPushButton:hover { border-color: #c8d8e8; }").arg(color.name()));
    };

    // ── Compact toggle cluster: Spots / Passive / Memories / Auto ─────
    // One row of name-on-button toggles replaces four label+button rows.
    // Visual state (green checked / red unchecked) carries the on/off
    // signal — no separate "Enabled"/"Disabled" text needed.
    {
        // Use the shared kSpotHubToggle style (matches VfoWidget's
        // kDspToggle) — see top of this file.
        auto makeToggle = [](const QString& label, bool checked,
                             const QString& tooltip = {}) {
            auto* btn = new QPushButton(label);
            btn->setCheckable(true);
            btn->setChecked(checked);
            btn->setFixedWidth(90);
            btn->setFixedHeight(26);
            if (!tooltip.isEmpty())
                btn->setToolTip(tooltip);
            btn->setStyleSheet(kSpotHubToggle);
            return btn;
        };

        auto* toggleRow = new QHBoxLayout;
        toggleRow->setSpacing(4);

        auto* spotsToggle = makeToggle("Spots", spotsEnabled);
        connect(spotsToggle, &QPushButton::toggled, this, [save](bool on) {
            save("IsSpotsEnabled", on ? "True" : "False");
        });
        toggleRow->addWidget(spotsToggle);

        auto* passiveToggle = makeToggle("Passive", passiveSpots,
            "Receive and render radio spots without sending spot add commands to the radio.");
        connect(passiveToggle, &QPushButton::toggled, this, [save](bool on) {
            save(SpotCommandPolicy::kPassiveSpotsModeKey, on ? "True" : "False");
        });
        toggleRow->addWidget(passiveToggle);

        auto* memoriesToggle = makeToggle("Memories", memoriesEnabled,
            "Show radio memory channels as a spot-like feed on the panadapter.");
        connect(memoriesToggle, &QPushButton::toggled, this, [save](bool on) {
            save("IsMemorySpotsEnabled", on ? "True" : "False");
        });
        toggleRow->addWidget(memoriesToggle);

        auto* autoModeToggle = makeToggle("Auto", autoMode,
            "Automatically switch slice mode when clicking a spot\n"
            "that includes mode information (e.g. CW, FT8, RTTY)");
        connect(autoModeToggle, &QPushButton::toggled, this, [save](bool on) {
            save("SpotAutoSwitchMode", on ? "True" : "False");
        });
        toggleRow->addWidget(autoModeToggle);

        // Signal History Markers (gold, voice signals).  Mirrors the
        // View-menu QAction; MainWindow listens to the signal below and
        // calls setChecked on the QAction, which re-fires the existing
        // toggled handler that owns the live apply + persistence logic.
        auto* signalsToggle = makeToggle("Signals", sHistorySignals,
            "Gold markers for detected voice-width signals on the panadapter.\n"
            "Same toggle as View → Signal History Markers.");
        connect(signalsToggle, &QPushButton::toggled, this,
                [this](bool on) { emit sHistoryEnabledToggled(on); });
        toggleRow->addWidget(signalsToggle);

        // QRM History Markers (red, persistent carriers / wideband).
        auto* qrmToggle = makeToggle("QRM", sHistoryQrm,
            "Red markers for persistent carriers and wideband interference.\n"
            "Same toggle as View → QRM History Markers.");
        connect(qrmToggle, &QPushButton::toggled, this,
                [this](bool on) { emit sHistoryQrmToggled(on); });
        toggleRow->addWidget(qrmToggle);

        // Clear-all action — non-checkable, but uses the same chrome as
        // the toggles so the row reads as one cluster.  Wipes DX spots,
        // memories feed, and S-History / QRM marker state in one click.
        auto* clearAllBtn = new QPushButton("Clear All");
        clearAllBtn->setFixedWidth(90);
        clearAllBtn->setFixedHeight(26);
        clearAllBtn->setToolTip(
            "Clear all DX cluster, RBN, WSJT-X, memory feed, signal history,\n"
            "and QRM history markers from the spectrum.");
        clearAllBtn->setStyleSheet(kSpotHubToggle);
        connect(clearAllBtn, &QPushButton::clicked, this, [this] {
            m_radioModel->sendCommand("spot clear");
            m_spotModel->clear();
            if (m_totalSpotsLabel)
                m_totalSpotsLabel->setText("0");
            emit spotsClearedAll();
            emit settingsChanged();
        });
        toggleRow->addWidget(clearAllBtn);

        toggleRow->addStretch();
        grid->addLayout(toggleRow, row++, 0, 1, 2);
    }

    // ── Levels slider ───────────────────────────────────────────────────
    grid->addWidget(new QLabel("Levels:"), row, 0);
    auto* levelsRow = new QHBoxLayout;
    auto* levelsSlider = new GuardedSlider(Qt::Horizontal);
    levelsSlider->setRange(1, 10);
    levelsSlider->setValue(levels);
    auto* levelsValue = new QLabel(QString::number(levels));
    levelsValue->setFixedWidth(24);
    levelsValue->setAlignment(Qt::AlignRight);
    levelsRow->addWidget(levelsSlider);
    levelsRow->addWidget(levelsValue);
    connect(levelsSlider, &QSlider::valueChanged, this, [levelsValue, save](int v) {
        levelsValue->setText(QString::number(v));
        save("SpotsMaxLevel", QString::number(v));
    });
    grid->addLayout(levelsRow, row++, 1);

    // ── Position slider ─────────────────────────────────────────────────
    grid->addWidget(new QLabel("Position:"), row, 0);
    auto* posRow = new QHBoxLayout;
    auto* posSlider = new GuardedSlider(Qt::Horizontal);
    posSlider->setRange(0, 100);
    posSlider->setValue(position);
    auto* posValue = new QLabel(QString::number(position));
    posValue->setFixedWidth(24);
    posValue->setAlignment(Qt::AlignRight);
    posRow->addWidget(posSlider);
    posRow->addWidget(posValue);
    connect(posSlider, &QSlider::valueChanged, this, [posValue, save](int v) {
        posValue->setText(QString::number(v));
        save("SpotsStartingHeightPercentage", QString::number(v));
    });
    grid->addLayout(posRow, row++, 1);

    // ── Font Size slider ────────────────────────────────────────────────
    grid->addWidget(new QLabel("Font Size:"), row, 0);
    auto* fontRow = new QHBoxLayout;
    auto* fontSlider = new GuardedSlider(Qt::Horizontal);
    fontSlider->setRange(8, 32);
    fontSlider->setValue(fontSize);
    auto* fontValue = new QLabel(QString::number(fontSize));
    fontValue->setFixedWidth(24);
    fontValue->setAlignment(Qt::AlignRight);
    fontRow->addWidget(fontSlider);
    fontRow->addWidget(fontValue);
    connect(fontSlider, &QSlider::valueChanged, this, [fontValue, save](int v) {
        fontValue->setText(QString::number(v));
        save("SpotFontSize", QString::number(v));
    });
    grid->addLayout(fontRow, row++, 1);

    // ── Spot Lifetime slider (non-linear: seconds → minutes → hours) ──
    grid->addWidget(new QLabel("Spot Lifetime:"), row, 0);
    auto* lifeRow = new QHBoxLayout;

    static QVector<int> lifeSteps;
    if (lifeSteps.isEmpty()) {
        for (int s = 10; s <= 55; s += 5)  lifeSteps.append(s);
        for (int m = 5;  m <= 55; m += 5)  lifeSteps.append(m * 60);
        for (int h = 1;  h <= 24; h++)      lifeSteps.append(h * 3600);
    }
    auto formatLifetime = [](int secs) -> QString {
        if (secs < 60)   return QString("%1 sec").arg(secs);
        if (secs < 3600) return QString("%1 min%2").arg(secs / 60).arg(secs / 60 == 1 ? "" : "s");
        int hrs = secs / 3600;
        if (hrs == 24) return QStringLiteral("1 day");
        return QString("%1 hr%2").arg(hrs).arg(hrs == 1 ? "" : "s");
    };
    int lifeIdx = 0;
    for (int i = 0; i < lifeSteps.size(); ++i)
        if (std::abs(lifeSteps[i] - lifetimeSec) < std::abs(lifeSteps[lifeIdx] - lifetimeSec))
            lifeIdx = i;

    auto* lifeSlider = new GuardedSlider(Qt::Horizontal);
    lifeSlider->setRange(0, lifeSteps.size() - 1);
    lifeSlider->setValue(lifeIdx);
    auto* lifeValue = new QLabel(formatLifetime(lifeSteps[lifeIdx]));
    lifeValue->setFixedWidth(90);
    lifeValue->setAlignment(Qt::AlignRight);
    lifeRow->addWidget(lifeSlider);
    lifeRow->addWidget(lifeValue);
    connect(lifeSlider, &QSlider::valueChanged, this, [lifeValue, formatLifetime, save](int idx) {
        int secs = lifeSteps.value(idx, 1800);
        lifeValue->setText(formatLifetime(secs));
        save("DxClusterSpotLifetimeSec", QString::number(secs));
    });
    grid->addLayout(lifeRow, row++, 1);

    // ── Override Colors + color picker ──────────────────────────────────
    grid->addWidget(new QLabel("Override Colors:"), row, 0);
    auto* colorRow = new QHBoxLayout;
    auto* overrideToggle = new QPushButton(overrideColors ? "Enabled" : "Disabled");
    overrideToggle->setCheckable(true);
    overrideToggle->setChecked(overrideColors);
    overrideToggle->setFixedWidth(80);
    overrideToggle->setStyleSheet(
        kSpotHubToggle);
    connect(overrideToggle, &QPushButton::toggled, this, [overrideToggle, save](bool on) {
        overrideToggle->setText(on ? "Enabled" : "Disabled");
        save("IsSpotsOverrideColorsEnabled", on ? "True" : "False");
    });
    colorRow->addWidget(overrideToggle);

    auto* colorBtn = new QPushButton;
    colorBtn->setFixedSize(24, 24);
    updateSwatch(colorBtn, spotColor);
    connect(colorBtn, &QPushButton::clicked, this, [this, colorBtn, updateSwatch, save, spotColor]() mutable {
        QColor c = QColorDialog::getColor(spotColor, this, "Spot Text Color");
        if (c.isValid()) {
            spotColor = c;
            updateSwatch(colorBtn, c);
            save("SpotsOverrideColor", c.name());
        }
    });
    colorRow->addWidget(colorBtn);
    colorRow->addStretch();
    grid->addLayout(colorRow, row++, 1);

    // ── Override Background + Auto + color picker ───────────────────────
    grid->addWidget(new QLabel("Override Background:"), row, 0);
    auto* bgRow = new QHBoxLayout;
    const QString& bgStyle = kSpotHubToggle;
    auto* bgEnabledBtn = new QPushButton("Enabled");
    bgEnabledBtn->setCheckable(true);
    bgEnabledBtn->setChecked(overrideBg);
    bgEnabledBtn->setFixedWidth(70);
    bgEnabledBtn->setStyleSheet(bgStyle);
    auto* bgAutoBtn = new QPushButton("Auto");
    bgAutoBtn->setCheckable(true);
    bgAutoBtn->setChecked(overrideBgAuto);
    bgAutoBtn->setFixedWidth(50);
    bgAutoBtn->setStyleSheet(bgStyle);
    connect(bgEnabledBtn, &QPushButton::toggled, this, [save](bool on) {
        save("IsSpotsOverrideBackgroundColorsEnabled", on ? "True" : "False");
    });
    connect(bgAutoBtn, &QPushButton::toggled, this, [save](bool on) {
        save("IsSpotsOverrideToAutoBackgroundColorEnabled", on ? "True" : "False");
    });
    bgRow->addWidget(bgEnabledBtn);
    bgRow->addWidget(bgAutoBtn);

    auto* bgColorBtn = new QPushButton;
    bgColorBtn->setFixedSize(24, 24);
    updateSwatch(bgColorBtn, bgColor);
    connect(bgColorBtn, &QPushButton::clicked, this, [this, bgColorBtn, updateSwatch, save, bgColor]() mutable {
        QColor c = QColorDialog::getColor(bgColor, this, "Spot Background Color");
        if (c.isValid()) {
            bgColor = c;
            updateSwatch(bgColorBtn, c);
            save("SpotsOverrideBgColor", c.name());
        }
    });
    bgRow->addWidget(bgColorBtn);
    bgRow->addStretch();
    grid->addLayout(bgRow, row++, 1);

    // ── Background Opacity slider ───────────────────────────────────────
    grid->addWidget(new QLabel("Background Opacity:"), row, 0);
    auto* opacRow = new QHBoxLayout;
    auto* opacSlider = new GuardedSlider(Qt::Horizontal);
    opacSlider->setRange(0, 100);
    opacSlider->setValue(bgOpacity);
    auto* opacValue = new QLabel(QString::number(bgOpacity));
    opacValue->setFixedWidth(24);
    opacValue->setAlignment(Qt::AlignRight);
    opacRow->addWidget(opacSlider);
    opacRow->addWidget(opacValue);
    connect(opacSlider, &QSlider::valueChanged, this, [opacValue, save](int v) {
        opacValue->setText(QString::number(v));
        save("SpotsBackgroundOpacity", QString::number(v));
    });
    grid->addLayout(opacRow, row++, 1);

    // ── Spot Lines ──────────────────────────────────────────────────────
    grid->addWidget(new QLabel("Spot Lines:"), row, 0);
    auto* spotLinesBtn = new QPushButton(spotLines ? "Enabled" : "Disabled");
    spotLinesBtn->setCheckable(true);
    spotLinesBtn->setChecked(spotLines);
    spotLinesBtn->setFixedWidth(80);
    spotLinesBtn->setToolTip("Show vertical lines from the spectrum up to each spot label.\nDisable during contests to reduce clutter.");
    spotLinesBtn->setStyleSheet(
        kSpotHubToggle);
    connect(spotLinesBtn, &QPushButton::toggled, this, [spotLinesBtn, save](bool on) {
        spotLinesBtn->setText(on ? "Enabled" : "Disabled");
        save("IsSpotsLinesEnabled", on ? "True" : "False");
    });
    grid->addWidget(spotLinesBtn, row++, 1, Qt::AlignLeft);

    // ── Total Spots ─────────────────────────────────────────────────────
    grid->addWidget(new QLabel("Total Spots:"), row, 0);
    m_totalSpotsLabel = new QLabel("0");
    m_totalSpotsLabel->setStyleSheet("QLabel { color: #c8d8e8; font-weight: bold; }");
    grid->addWidget(m_totalSpotsLabel, row++, 1);

    layout->addLayout(grid);

    // ── Two-column section below the divider ────────────────────────────
    // Left column: DXCC Coloring (#330)
    // Right column: Signal History controls
    // Top border on each column header forms the visual divider line.
    auto* twoColRow = new QHBoxLayout;
    twoColRow->setSpacing(16);
    auto* leftCol  = new QVBoxLayout;
    auto* rightCol = new QVBoxLayout;
    leftCol->setSpacing(4);
    rightCol->setSpacing(4);

    // ── DXCC Coloring section (#330) — left column ──────────────────────
    {
        auto* dxccTitle = new QLabel("DXCC Coloring");
        dxccTitle->setAlignment(Qt::AlignCenter);
        dxccTitle->setStyleSheet(
            "QLabel { font-size: 13px; font-weight: bold; color: #80b0d0; "
            "border-top: 1px solid #304050; padding-top: 8px; margin-top: 6px; }");
        leftCol->addWidget(dxccTitle);

        auto* dxccGrid = new QGridLayout;
        dxccGrid->setColumnStretch(1, 1);
        int drow = 0;

        bool dxccEnabled = m_dxccProvider ? m_dxccProvider->isEnabled() : false;
        auto* dxccToggle = new QPushButton(dxccEnabled ? "Enabled" : "Disabled");
        dxccToggle->setCheckable(true);
        dxccToggle->setChecked(dxccEnabled);
        dxccToggle->setFixedWidth(80);
        dxccToggle->setStyleSheet(
            kSpotHubToggle);
        connect(dxccToggle, &QPushButton::toggled, this, [this, dxccToggle, save](bool on) {
            dxccToggle->setText(on ? "Enabled" : "Disabled");
            save("IsDxccColoringEnabled", on ? "True" : "False");
            if (m_dxccProvider) m_dxccProvider->setEnabled(on);
        });
        dxccGrid->addWidget(new QLabel("DXCC Colors:"), drow, 0);
        dxccGrid->addWidget(dxccToggle, drow++, 1, Qt::AlignLeft);

        // ADIF file picker
        const QString savedAdif = AppSettings::instance().value("DxccAdifFilePath", "").toString();
        auto* adifPathLabel = new QLabel(savedAdif.isEmpty() ? "(none)" : QFileInfo(savedAdif).fileName());
        adifPathLabel->setStyleSheet("QLabel { color: #90a8b8; font-size: 11px; }");
        adifPathLabel->setMaximumWidth(200);
        auto* browseBtn = new QPushButton("Browse\xe2\x80\xa6");
        browseBtn->setFixedWidth(80);
        auto* adifRow = new QHBoxLayout;
        adifRow->addWidget(adifPathLabel);
        adifRow->addWidget(browseBtn);
        adifRow->addStretch();
        dxccGrid->addWidget(new QLabel("Log File (ADIF):"), drow, 0);
        dxccGrid->addLayout(adifRow, drow++, 1);

        // Stats label
        m_dxccStatsLabel = new QLabel(m_dxccProvider && m_dxccProvider->qsoCount() > 0
            ? QString("%1 QSOs / %2 entities").arg(m_dxccProvider->qsoCount()).arg(m_dxccProvider->entityCount())
            : "(no log loaded)");
        m_dxccStatsLabel->setStyleSheet("QLabel { color: #90c890; font-size: 11px; }");
        dxccGrid->addWidget(new QLabel("Imported:"), drow, 0);
        dxccGrid->addWidget(m_dxccStatsLabel, drow++, 1);

        // Colour swatches
        auto* swatchRow = new QHBoxLayout;
        struct SwatchDef { const char* tip; QColor* colPtr; const char* key; };
        SwatchDef swatches[] = {
            { "New DXCC", m_dxccProvider ? &m_dxccProvider->colorNewDxcc : nullptr, "DxccColorNewEntity" },
            { "New Band",  m_dxccProvider ? &m_dxccProvider->colorNewBand  : nullptr, "DxccColorNewBand"   },
            { "New Mode",  m_dxccProvider ? &m_dxccProvider->colorNewMode  : nullptr, "DxccColorNewMode"   },
            { "Worked",    m_dxccProvider ? &m_dxccProvider->colorWorked   : nullptr, "DxccColorWorked"    },
        };
        for (const auto& sw : swatches) {
            auto* col = new QVBoxLayout;
            auto* lbl = new QLabel(sw.tip);
            lbl->setAlignment(Qt::AlignCenter);
            lbl->setStyleSheet("QLabel { color: #809090; font-size: 10px; }");
            auto* btn = new QPushButton;
            btn->setFixedSize(28, 22);
            const QColor initCol = sw.colPtr ? *sw.colPtr : QColor(Qt::gray);
            updateSwatch(btn, initCol);
            const QString key = sw.key;
            QColor* colPtr = sw.colPtr;
            connect(btn, &QPushButton::clicked, this, [this, btn, key, colPtr, updateSwatch, save]() {
                const QColor cur = colPtr ? *colPtr : QColor(Qt::gray);
                QColor c = QColorDialog::getColor(cur, this, "Pick Colour");
                if (!c.isValid()) return;
                if (colPtr) *colPtr = c;
                updateSwatch(btn, c);
                save(key, c.name());
            });
            col->addWidget(lbl);
            col->addWidget(btn, 0, Qt::AlignCenter);
            swatchRow->addLayout(col);
        }
        swatchRow->addStretch();
        dxccGrid->addWidget(new QLabel("Colors:"), drow, 0);
        dxccGrid->addLayout(swatchRow, drow++, 1);

        leftCol->addLayout(dxccGrid);
        leftCol->addStretch();

        // Wire browse button — always arms the file watcher automatically so
        // spot colours update whenever the user exports a new log (#logbook-autoreload).
        connect(browseBtn, &QPushButton::clicked, this, [this, adifPathLabel, save](bool) {
            const QString path = QFileDialog::getOpenFileName(
                this, "Select ADIF Log File", QDir::homePath(),
                "ADIF Log Files (*.adi *.adif);;All Files (*)");
            if (path.isEmpty()) return;
            adifPathLabel->setText(QFileInfo(path).fileName());
            save("DxccAdifFilePath", path);
            if (m_dxccProvider) {
                m_dxccProvider->importAdifFile(path);   // importStarted → "Updating…" via signal below
                m_dxccProvider->setAutoReload(true, path);  // always watch after selection
            }
        });

        // importStarted → "Updating…", importFinished → live stats
        if (m_dxccProvider) {
            connect(m_dxccProvider, &DxccColorProvider::importStarted,
                    this, [this]() {
                if (m_dxccStatsLabel)
                    m_dxccStatsLabel->setText("Updating\xe2\x80\xa6");
            });
            connect(m_dxccProvider, &DxccColorProvider::importFinished,
                    this, [this](int qsos, int entities) {
                if (m_dxccStatsLabel)
                    m_dxccStatsLabel->setText(
                        QString("%1 QSOs / %2 entities").arg(qsos).arg(entities));
            });
        }
    }

    // ── Signal History section — right column ───────────────────────────
    // The [Signals] / [QRM] toggles in the top row gate marker visibility;
    // these controls tune the classification/expiry timings.
    {
        auto* shTitle = new QLabel("Signal History");
        shTitle->setAlignment(Qt::AlignCenter);
        shTitle->setStyleSheet(
            "QLabel { font-size: 13px; font-weight: bold; color: #80b0d0; "
            "border-top: 1px solid #304050; padding-top: 8px; margin-top: 6px; }");
        rightCol->addWidget(shTitle);

        auto* shGrid = new QGridLayout;
        shGrid->setColumnStretch(1, 1);
        int shr = 0;

        // Marker Lifetime — how long inactive markers persist before purge
        // (replaces the historical 60 s constant in MainWindow::expireSHistoryMarkers).
        const int lifetimeS = AppSettings::instance()
            .value("SHistoryLifetimeS", 60).toInt();
        shGrid->addWidget(new QLabel("Marker Lifetime:"), shr, 0);
        auto* lifeRow = new QHBoxLayout;
        auto* lifeSlider = new GuardedSlider(Qt::Horizontal);
        lifeSlider->setRange(15, 300);
        lifeSlider->setValue(std::clamp(lifetimeS, 15, 300));
        lifeSlider->setToolTip(
            "How long an inactive S-History marker persists before being\n"
            "removed.  Default 60 s.");
        auto* lifeValue = new QLabel(QString("%1 s").arg(lifeSlider->value()));
        lifeValue->setFixedWidth(48);
        lifeValue->setAlignment(Qt::AlignRight);
        lifeRow->addWidget(lifeSlider);
        lifeRow->addWidget(lifeValue);
        connect(lifeSlider, &QSlider::valueChanged, this, [lifeValue, save](int v) {
            lifeValue->setText(QString("%1 s").arg(v));
            save("SHistoryLifetimeS", QString::number(v));
        });
        shGrid->addLayout(lifeRow, shr++, 1);

        // QRM Gate Time — minimum age before a narrow/wideband signal can be
        // classified as QRM.  Voice-width signals use a separate longer gate.
        const int qrmGateS = AppSettings::instance()
            .value("SHistoryQrmGateS", 6).toInt();
        shGrid->addWidget(new QLabel("QRM Gate:"), shr, 0);
        auto* qrmRow = new QHBoxLayout;
        auto* qrmSlider = new GuardedSlider(Qt::Horizontal);
        qrmSlider->setRange(3, 30);
        qrmSlider->setValue(std::clamp(qrmGateS, 3, 30));
        qrmSlider->setToolTip(
            "How long a narrow carrier or wideband signal must persist\n"
            "before being classified as QRM.  Default 6 s.");
        auto* qrmValue = new QLabel(QString("%1 s").arg(qrmSlider->value()));
        qrmValue->setFixedWidth(48);
        qrmValue->setAlignment(Qt::AlignRight);
        qrmRow->addWidget(qrmSlider);
        qrmRow->addWidget(qrmValue);
        connect(qrmSlider, &QSlider::valueChanged, this, [qrmValue, save](int v) {
            qrmValue->setText(QString("%1 s").arg(v));
            save("SHistoryQrmGateS", QString::number(v));
        });
        shGrid->addLayout(qrmRow, shr++, 1);

        // Edge Threshold — softer noise-above-floor cutoff used by the slope
        // edge walk to refine the carrier-side mode edge.  Lower values pull
        // the marker closer to the carrier (more aggressive).  Steps in
        // 0.5 dB increments via a 10× int slider.
        const int edgeDbInt = static_cast<int>(std::lround(
            AppSettings::instance().value("SHistorySoftEdgeDb", "3.0").toFloat() * 10.0f));
        shGrid->addWidget(new QLabel("Edge Threshold:"), shr, 0);
        auto* edgeRow = new QHBoxLayout;
        auto* edgeSlider = new GuardedSlider(Qt::Horizontal);
        edgeSlider->setRange(10, 100);  // 1.0–10.0 dB in 0.1 dB units
        edgeSlider->setSingleStep(5);   // snap-feel: 0.5 dB notches
        edgeSlider->setValue(std::clamp(edgeDbInt, 10, 100));
        edgeSlider->setToolTip(
            "Threshold above noise floor for the slope edge walk that\n"
            "refines the SHistory carrier-side edge.  Lower = closer to\n"
            "the carrier but more sensitive to noise.  Default 3.0 dB.");
        auto* edgeValue = new QLabel(QString::number(edgeSlider->value() / 10.0, 'f', 1) + " dB");
        edgeValue->setFixedWidth(56);
        edgeValue->setAlignment(Qt::AlignRight);
        edgeRow->addWidget(edgeSlider);
        edgeRow->addWidget(edgeValue);
        connect(edgeSlider, &QSlider::valueChanged, this, [edgeValue, save](int v) {
            const double db = v / 10.0;
            edgeValue->setText(QString::number(db, 'f', 1) + " dB");
            save("SHistorySoftEdgeDb", QString::number(db, 'f', 1));
        });
        shGrid->addLayout(edgeRow, shr++, 1);

        // Marker colour swatches — mirrors the DXCC swatch pattern in the
        // left column.  Two pickers: Signals (voice) and QRM.
        QColor signalsColor(AppSettings::instance()
            .value("SHistoryColorSignals", "#FFC800").toString());
        QColor qrmColor(AppSettings::instance()
            .value("SHistoryColorQrm",     "#FF0000").toString());

        auto* shSwatchRow = new QHBoxLayout;
        struct ShSwatchDef { const char* label; QColor* colPtr; const char* key; };
        ShSwatchDef shSwatches[] = {
            { "Signals", &signalsColor, "SHistoryColorSignals" },
            { "QRM",     &qrmColor,     "SHistoryColorQrm"     },
        };
        for (const auto& sw : shSwatches) {
            auto* col = new QVBoxLayout;
            auto* lbl = new QLabel(sw.label);
            lbl->setAlignment(Qt::AlignCenter);
            lbl->setStyleSheet("QLabel { color: #809090; font-size: 10px; }");
            auto* btn = new QPushButton;
            btn->setFixedSize(28, 22);
            updateSwatch(btn, *sw.colPtr);
            const QString key = sw.key;
            QColor* colPtr = sw.colPtr;
            connect(btn, &QPushButton::clicked, this,
                    [this, btn, key, colPtr, updateSwatch, save]() {
                QColor c = QColorDialog::getColor(*colPtr, this, "Pick Colour");
                if (!c.isValid()) return;
                *colPtr = c;
                updateSwatch(btn, c);
                save(key, c.name());
            });
            col->addWidget(lbl);
            col->addWidget(btn, 0, Qt::AlignCenter);
            shSwatchRow->addLayout(col);
        }
        shSwatchRow->addStretch();
        shGrid->addWidget(new QLabel("Colors:"), shr, 0);
        shGrid->addLayout(shSwatchRow, shr++, 1);

        // Snap-to-step toggle — rounds SHistory click-to-tune to the
        // nearest multiple of the active slice's step size.  Compensates
        // for the detector edge-bin imprecision (typically 100–300 Hz off
        // the carrier).
        const bool snapEnabled = AppSettings::instance()
            .value("SHistorySnapToStep", "False").toString() == "True";
        auto* snapToggle = new QPushButton(snapEnabled ? "Enabled" : "Disabled");
        snapToggle->setCheckable(true);
        snapToggle->setChecked(snapEnabled);
        snapToggle->setFixedWidth(80);
        snapToggle->setStyleSheet(kSpotHubToggle);
        snapToggle->setToolTip(
            "Round SHistory click-to-tune to the nearest multiple of the\n"
            "active slice's step size.  Hides the small carrier offset that\n"
            "comes from detecting voice on the panadapter.");
        connect(snapToggle, &QPushButton::toggled, this, [snapToggle, save](bool on) {
            snapToggle->setText(on ? "Enabled" : "Disabled");
            save("SHistorySnapToStep", on ? "True" : "False");
        });
        shGrid->addWidget(new QLabel("Snap to Step:"), shr, 0);
        shGrid->addWidget(snapToggle, shr++, 1, Qt::AlignLeft);

        rightCol->addLayout(shGrid);
        rightCol->addStretch();
    }

    twoColRow->addLayout(leftCol, 1);
    twoColRow->addLayout(rightCol, 1);
    layout->addLayout(twoColRow);

    layout->addStretch();

    tabs->addTab(page, "Display");
}

void DxClusterDialog::setFramelessMode(bool on)
{
    const QRect geom = geometry();
    const bool wasVisible = isVisible();

    Qt::WindowFlags flags = (windowFlags() & ~Qt::WindowType_Mask) | Qt::Dialog;
    flags.setFlag(Qt::FramelessWindowHint, on);
    setWindowFlags(flags);
    if (wasVisible) {
        setGeometry(geom);
    }

    if (m_titleBar) {
        m_titleBar->setVisible(on);
    }
    if (m_outerLayout) {
        m_outerLayout->setContentsMargins(0, 0, 0, 0);
    }

    if (wasVisible) {
        show();
    }
}

void DxClusterDialog::setTotalSpots(int count)
{
    if (m_totalSpotsLabel)
        m_totalSpotsLabel->setText(QString::number(count));
}

void DxClusterDialog::flushSpotBatch()
{
    if (m_spotBatch.isEmpty()) return;

    auto isAtBottom = [](QAbstractScrollArea* w) {
        auto* sb = w->verticalScrollBar();
        return sb->value() >= sb->maximum() - 2;
    };
    bool follow = isAtBottom(m_spotTable);

    m_spotModel->addSpots(m_spotBatch);
    m_spotBatch.clear();

    if (follow)
        m_spotTable->scrollToBottom();
}

void DxClusterDialog::updateStatus()
{
    // Cluster status
    if (m_client->isConnected()) {
        m_statusLabel->setText(QString("Connected to %1:%2").arg(m_client->host()).arg(m_client->port()));
        m_statusLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 11px; }");
        m_connectBtn->setText("Disconnect");
        m_cmdEdit->setEnabled(true);
        m_sendBtn->setEnabled(true);
    } else {
        m_statusLabel->setText("Disconnected");
        m_statusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
        m_connectBtn->setText("Connect");
        m_cmdEdit->setEnabled(false);
        m_sendBtn->setEnabled(false);
    }
    // RBN status
    if (m_rbnClient->isConnected()) {
        m_rbnStatusLabel->setText(QString("Connected to %1:%2").arg(m_rbnClient->host()).arg(m_rbnClient->port()));
        m_rbnStatusLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 11px; }");
        m_rbnConnectBtn->setText("Disconnect");
        m_rbnCmdEdit->setEnabled(true);
        m_rbnSendBtn->setEnabled(true);
    } else {
        m_rbnStatusLabel->setText("Disconnected");
        m_rbnStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
        m_rbnConnectBtn->setText("Connect");
        m_rbnCmdEdit->setEnabled(false);
        m_rbnSendBtn->setEnabled(false);
    }
    // WSJT-X status
    if (m_wsjtxClient->isListening()) {
        m_wsjtxStatusLabel->setText(QString("Listening on port %1").arg(m_wsjtxPortSpin->value()));
        m_wsjtxStatusLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 11px; }");
        m_wsjtxStartBtn->setText("Stop");
    } else {
        m_wsjtxStatusLabel->setText("Stopped");
        m_wsjtxStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
        m_wsjtxStartBtn->setText("Start");
    }
    // POTA status
    if (m_potaClient->isPolling()) {
        m_potaStatusLabel->setText("Polling...");
        m_potaStatusLabel->setStyleSheet("QLabel { color: #00b4d8; font-size: 11px; }");
        m_potaStartBtn->setText("Stop");
    } else {
        m_potaStatusLabel->setText("Stopped");
        m_potaStatusLabel->setStyleSheet("QLabel { color: #808080; font-size: 11px; }");
        m_potaStartBtn->setText("Start");
    }
}

} // namespace AetherSDR
