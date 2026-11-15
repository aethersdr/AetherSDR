#pragma once

#ifdef HAVE_WEBSOCKETS

#include <QAbstractTableModel>
#include <QDateTime>
#include <QHash>
#include <QTimer>
#include <QVector>
#include "core/FreeDvClient.h"

namespace AetherSDR {

class FreeDvReporterModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Col {
        Callsign = 0,
        Locator,
        Km,
        Hdg,
        Version,
        MHz,
        Mode,
        Status,
        Msg,
        LastTx,
        RxCall,
        Snr,
        LastUpdate,
        Count
    };

    // Timed highlight type for a row. TX uses info.status directly (no timer);
    // RX and Msg are stamped and expire after their respective timeouts.
    enum class HighlightType { None, RX, Msg };

    explicit FreeDvReporterModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

public slots:
    void onStationsCleared();
    void onStationUpdated(const QString& sid, const AetherSDR::FreeDvClient::StationInfo& info);
    void onStationRemoved(const QString& sid);
    void setMyGrid(const QString& grid);

private slots:
    void onHighlightTick();

private:
    struct Row {
        QString                       sid;
        FreeDvClient::StationInfo     info;
        int                           km{-1};
        int                           hdg{-1};
        HighlightType                 highlightType{HighlightType::None};
        QDateTime                     highlightSince;
    };

    bool isHighlightActive(const Row& row) const;
    void recomputeDistances(Row& row) const;

    QVector<Row>        m_rows;
    QHash<QString, int> m_sidIndex;  // sid → row index (kept in sync)
    QString             m_myGrid;
    QTimer*             m_highlightTimer{nullptr};

    void rebuildIndex();
};

} // namespace AetherSDR

#endif // HAVE_WEBSOCKETS
