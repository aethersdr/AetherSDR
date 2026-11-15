#ifdef HAVE_WEBSOCKETS

#include "FreeDvReporterModel.h"
#include "core/MaidenheadLocator.h"
#include "core/ThemeManager.h"

#include <QBrush>
#include <QDateTime>
#include <QTimer>

namespace AetherSDR {

namespace {

// Timeouts (seconds)
constexpr int kRxSec  = 5;  // RX highlight expires 5s after last rx_report
constexpr int kMsgSec = 5;  // message_update / new-station arrival

} // namespace

// ── Construction ─────────────────────────────────────────────────────────────

FreeDvReporterModel::FreeDvReporterModel(QObject* parent)
    : QAbstractTableModel(parent)
{
    m_highlightTimer = new QTimer(this);
    m_highlightTimer->setInterval(250);
    connect(m_highlightTimer, &QTimer::timeout,
            this, &FreeDvReporterModel::onHighlightTick);
    m_highlightTimer->start();

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this] {
                if (!m_rows.isEmpty())
                    emit dataChanged(index(0, 0),
                                     index(m_rows.size() - 1, Col::Count - 1),
                                     {Qt::BackgroundRole, Qt::ForegroundRole});
            });
}

// ── QAbstractTableModel overrides ────────────────────────────────────────────

int FreeDvReporterModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return m_rows.size();
}

int FreeDvReporterModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) return 0;
    return Col::Count;
}

QVariant FreeDvReporterModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    const Row& row = m_rows.at(index.row());
    const FreeDvClient::StationInfo& info = row.info;
    const int col = index.column();

    if (role == Qt::DisplayRole || role == Qt::UserRole) {
        switch (col) {
        case Callsign:
            return info.callsign;
        case Locator:
            return info.gridSquare;
        case Km:
            if (role == Qt::UserRole) return row.km;
            return row.km < 0 ? QStringLiteral("---") : QString::number(row.km);
        case Hdg:
            if (role == Qt::UserRole) return row.hdg;
            return row.hdg < 0 ? QStringLiteral("---") : QString::number(row.hdg);
        case Version:
            return info.version;
        case MHz:
            if (role == Qt::UserRole) return info.freqMhz;
            return info.freqMhz > 0.0
                ? QString::number(info.freqMhz, 'f', 4)
                : QStringLiteral("---");
        case Mode:
            return info.mode;
        case Status:
            return info.status;
        case Msg:
            return info.message;
        case LastTx:
            return info.lastTxTime.isValid()
                ? info.lastTxTime.toUTC().toString("MM/dd/yyyy HH:mm:ss 'Z'")
                : QString{};
        case RxCall:
            return info.rxCallsign;
        case Snr:
            if (role == Qt::UserRole) return static_cast<double>(info.snr);
            return info.snr <= -99.0f
                ? QStringLiteral("---")
                : QString::number(static_cast<int>(info.snr));
        case LastUpdate:
            return info.lastUpdate.isValid()
                ? info.lastUpdate.toUTC().toString("MM/dd/yyyy HH:mm:ss 'Z'")
                : QString{};
        default:
            return {};
        }
    }

    if (role == Qt::BackgroundRole) {
        if (info.status == u"TX")
            return QBrush(ThemeManager::instance().color(QStringLiteral("color.highlight.tx")));
        if (isHighlightActive(row)) {
            const QString token = row.highlightType == HighlightType::RX
                                  ? QStringLiteral("color.highlight.rx")
                                  : QStringLiteral("color.highlight.message");
            return QBrush(ThemeManager::instance().color(token));
        }
        return {};
    }

    if (role == Qt::ForegroundRole) {
        if (info.status == u"TX" || isHighlightActive(row))
            return QBrush(ThemeManager::instance().color(QStringLiteral("color.highlight.fg")));
        return {};
    }

    return {};
}

QVariant FreeDvReporterModel::headerData(int section, Qt::Orientation orientation,
                                          int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case Callsign:   return QStringLiteral("Callsign");
    case Locator:    return QStringLiteral("Locator");
    case Km:         return QStringLiteral("km");
    case Hdg:        return QStringLiteral("Hdg");
    case Version:    return QStringLiteral("Version");
    case MHz:        return QStringLiteral("MHz");
    case Mode:       return QStringLiteral("TX Mode");
    case Status:     return QStringLiteral("Status");
    case Msg:        return QStringLiteral("Message");
    case LastTx:     return QStringLiteral("Last TX");
    case RxCall:     return QStringLiteral("RX Call");
    case Snr:        return QStringLiteral("SNR");
    case LastUpdate: return QStringLiteral("Last Update");
    default:         return {};
    }
}

Qt::ItemFlags FreeDvReporterModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

// ── Highlight helpers ─────────────────────────────────────────────────────────

bool FreeDvReporterModel::isHighlightActive(const Row& row) const
{
    if (row.highlightType == HighlightType::None) return false;
    const int elapsed = row.highlightSince.secsTo(QDateTime::currentDateTimeUtc());
    if (row.highlightType == HighlightType::Msg)
        return elapsed < kMsgSec;
    return elapsed < kRxSec;
}

void FreeDvReporterModel::onHighlightTick()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < m_rows.size(); ++i) {
        Row& row = m_rows[i];

        // Expire timed RX / Msg highlights
        if (row.highlightType != HighlightType::None && !isHighlightActive(row)) {
            row.highlightType = HighlightType::None;
            emit dataChanged(index(i, 0), index(i, Col::Count - 1),
                             {Qt::BackgroundRole, Qt::ForegroundRole});
        }

    }
}

// ── Public slots ─────────────────────────────────────────────────────────────

void FreeDvReporterModel::onStationsCleared()
{
    if (m_rows.isEmpty()) return;
    beginResetModel();
    m_rows.clear();
    m_sidIndex.clear();
    endResetModel();
}

void FreeDvReporterModel::setMyGrid(const QString& grid)
{
    m_myGrid = grid.trimmed().toUpper();
    for (int i = 0; i < m_rows.size(); ++i)
        recomputeDistances(m_rows[i]);
    if (!m_rows.isEmpty())
        emit dataChanged(index(0, Col::Km), index(m_rows.size() - 1, Col::Hdg));
}

void FreeDvReporterModel::recomputeDistances(Row& row) const
{
    if (m_myGrid.isEmpty() || row.info.gridSquare.isEmpty()) {
        row.km  = -1;
        row.hdg = -1;
        return;
    }
    double km = 0.0, bearing = 0.0;
    if (MaidenheadLocator::gridDistance(m_myGrid, row.info.gridSquare, km, bearing)) {
        row.km  = static_cast<int>(km);
        row.hdg = static_cast<int>(bearing);
    } else {
        row.km  = -1;
        row.hdg = -1;
    }
}

void FreeDvReporterModel::onStationUpdated(const QString& sid,
                                            const FreeDvClient::StationInfo& info)
{
    auto it = m_sidIndex.find(sid);
    if (it != m_sidIndex.end()) {
        const int rowIdx = it.value();
        Row& row = m_rows[rowIdx];
        const FreeDvClient::StationInfo& prev = row.info;

        // Determine highlight from what changed
        if (info.status == u"TX" && prev.status != u"TX") {
            // Live TX start — colour driven from info.status in data(); clear timed highlight
            row.highlightType = HighlightType::None;
        } else if (info.status == u"RX" && prev.status == u"TX") {
            // TX ended — clear immediately; no timed highlight on revert
            row.highlightType = HighlightType::None;
        } else if (info.message != prev.message) {
            row.highlightType  = HighlightType::Msg;
            row.highlightSince = QDateTime::currentDateTimeUtc();
        } else if (!info.rxCallsign.isEmpty() && prev.rxCallsign.isEmpty()) {
            // EOO decoded — rxCallsign transitioned "" → callsign; clear teal immediately
            row.highlightType = HighlightType::None;
        } else if (row.highlightType == HighlightType::RX) {
            // Already receiving — any update refreshes the 5s window
            row.highlightSince = QDateTime::currentDateTimeUtc();
        } else if (info.rxCallsign != prev.rxCallsign || info.snr != prev.snr) {
            // New over started (rxCallsign just cleared) or first SNR report
            row.highlightType  = HighlightType::RX;
            row.highlightSince = QDateTime::currentDateTimeUtc();
        }

        row.info = info;
        recomputeDistances(row);
        emit dataChanged(index(rowIdx, 0), index(rowIdx, Col::Count - 1));
    } else {
        // New station — purple flash for arrival (same timeout as message updates)
        const int newRow = m_rows.size();
        beginInsertRows({}, newRow, newRow);
        Row r;
        r.sid            = sid;
        r.info           = info;
        r.highlightType  = HighlightType::Msg;
        r.highlightSince = QDateTime::currentDateTimeUtc();
        recomputeDistances(r);
        m_rows.append(r);
        m_sidIndex[sid] = newRow;
        endInsertRows();
    }
}

void FreeDvReporterModel::onStationRemoved(const QString& sid)
{
    auto it = m_sidIndex.find(sid);
    if (it == m_sidIndex.end()) return;
    const int row = it.value();
    beginRemoveRows({}, row, row);
    m_rows.removeAt(row);
    m_sidIndex.remove(sid);
    // Renumber indices for rows after the removed one
    for (auto& idx : m_sidIndex) {
        if (idx > row) --idx;
    }
    endRemoveRows();
}

void FreeDvReporterModel::rebuildIndex()
{
    m_sidIndex.clear();
    for (int i = 0; i < m_rows.size(); ++i)
        m_sidIndex[m_rows[i].sid] = i;
}

} // namespace AetherSDR

#endif // HAVE_WEBSOCKETS
