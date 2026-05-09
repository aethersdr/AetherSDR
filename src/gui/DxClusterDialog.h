#pragma once

#include <QDialog>
#include <QAbstractTableModel>
#include <QSortFilterProxyModel>
#include <QSet>
#include <QVector>
#include "core/DxClusterClient.h"
#include "core/DxccColorProvider.h"
#include "core/WsjtxClient.h"
#include "core/SpotCollectorClient.h"
#include "core/PotaClient.h"
#ifdef HAVE_WEBSOCKETS
#include "core/FreeDvClient.h"
#endif

class QLineEdit;
class QSpinBox;
class QPushButton;
class QLabel;
class QCheckBox;
class QPlainTextEdit;
class QTabWidget;
class QTableView;

namespace AetherSDR {

class DxClusterClient;
class RadioModel;

// ── Spot list table model ───────────────────────────────────────────────────

class SpotTableModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { ColTime, ColFreq, ColDxCall, ColComment, ColSpotter, ColBand, ColMode, ColSource, ColCount };
    static QString extractMode(const QString& comment);

    explicit SpotTableModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    int rowCount(const QModelIndex& = {}) const override { return m_spots.size(); }
    int columnCount(const QModelIndex& = {}) const override { return ColCount; }
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void addSpot(const DxSpot& spot);
    void addSpots(const QVector<DxSpot>& spots);
    void clear();
    void setMaxSpots(int max) { m_maxSpots = max; }
    const DxSpot* spotAt(int row) const;

private:
    static QString bandForFreq(double mhz);

    QVector<DxSpot> m_spots;
    int m_maxSpots{500};
};

// ── Band filter proxy ───────────────────────────────────────────────────────

class BandFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit BandFilterProxy(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {}

    void setBandVisible(const QString& band, bool visible);
    bool isBandVisible(const QString& band) const { return !m_hiddenBands.contains(band); }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    QSet<QString> m_hiddenBands;
};

// ── Dialog ──────────────────────────────────────────────────────────────────

class DxClusterDialog : public QDialog {
    Q_OBJECT

public:
    explicit DxClusterDialog(DxClusterClient* clusterClient, DxClusterClient* rbnClient,
                             WsjtxClient* wsjtxClient, SpotCollectorClient* spotCollectorClient,
                             PotaClient* potaClient,
#ifdef HAVE_WEBSOCKETS
                             FreeDvClient* freedvClient,
#endif
                             RadioModel* radioModel,
                             DxccColorProvider* dxccProvider,
                             QWidget* parent = nullptr);

    void updateStatus();
    void setTotalSpots(int count);

signals:
    void connectRequested(const QString& host, quint16 port, const QString& callsign);
    void disconnectRequested();
    void rbnConnectRequested(const QString& host, quint16 port, const QString& callsign);
    void rbnDisconnectRequested();
    void wsjtxStartRequested(const QString& address, quint16 port);
    void wsjtxStopRequested();
    void spotCollectorStartRequested(quint16 port);
    void spotCollectorStopRequested();
    void potaStartRequested(int intervalSec);
    void potaStopRequested();
#ifdef HAVE_WEBSOCKETS
    void freedvStartRequested();
    void freedvStopRequested();
    void freedvReportingToggled(bool enabled);
    void freedvMessageChanged(const QString& message);
#endif
    void wsjtxSpotFiltered(const DxSpot& spot);  // WSJT-X spot after filter+color
    void tuneRequested(double freqMhz, const QString& spotMode, const QString& comment);
    void settingsChanged();
    void spotsClearedAll();

private:
    void buildClusterTab(QTabWidget* tabs);
    void buildRbnTab(QTabWidget* tabs);
    void buildWsjtxTab(QTabWidget* tabs);
    void buildSpotCollectorTab(QTabWidget* tabs);
    void buildPotaTab(QTabWidget* tabs);
#ifdef HAVE_WEBSOCKETS
    void buildFreeDvTab(QTabWidget* tabs);
#endif
    void buildSpotListTab(QTabWidget* tabs);
    void buildDisplayTab(QTabWidget* tabs);
    void loadLogFiles(const QString& clusterLog, const QString& rbnLog,
                      const QString& wsjtxLog, const QString& potaLog,
                      const QString& freedvLog = {});

    DxClusterClient*      m_client;
    DxClusterClient*      m_rbnClient;
    WsjtxClient*          m_wsjtxClient;
    SpotCollectorClient*  m_spotCollectorClient;
    PotaClient*           m_potaClient;
#ifdef HAVE_WEBSOCKETS
    FreeDvClient*    m_freedvClient;
#endif
    RadioModel*      m_radioModel;

    // Cluster tab
    QLineEdit*      m_hostEdit;
    QSpinBox*       m_portSpin;
    QLineEdit*      m_callEdit;
    QPushButton*    m_connectBtn;
    QPushButton*    m_autoConnectBtn;
    QLabel*         m_statusLabel;
    QPlainTextEdit* m_console;
    QLineEdit*      m_cmdEdit;
    QPushButton*    m_sendBtn;

    // RBN tab
    QLineEdit*      m_rbnHostEdit;
    QSpinBox*       m_rbnPortSpin;
    QLineEdit*      m_rbnCallEdit;
    QPushButton*    m_rbnConnectBtn;
    QPushButton*    m_rbnAutoConnectBtn;
    QLabel*         m_rbnStatusLabel;
    QPlainTextEdit* m_rbnConsole;
    QLineEdit*      m_rbnCmdEdit;
    QPushButton*    m_rbnSendBtn;

    // WSJT-X tab
    QLineEdit*      m_wsjtxAddrEdit;
    QSpinBox*       m_wsjtxPortSpin;
    QPushButton*    m_wsjtxStartBtn;
    QPushButton*    m_wsjtxAutoStartBtn;
    QLabel*         m_wsjtxStatusLabel;
    QPlainTextEdit* m_wsjtxConsole;
    QCheckBox*      m_wsjtxFilterCQ;
    QCheckBox*      m_wsjtxFilterPOTA;
    QCheckBox*      m_wsjtxFilterCallingMe;

    // SpotCollector tab
    QSpinBox*       m_scPortSpin;
    QPushButton*    m_scStartBtn;
    QPushButton*    m_scAutoStartBtn;
    QLabel*         m_scStatusLabel;
    QPlainTextEdit* m_scConsole;

    // POTA tab
    QSpinBox*       m_potaIntervalSpin;
    QPushButton*    m_potaStartBtn;
    QPushButton*    m_potaAutoStartBtn;
    QLabel*         m_potaStatusLabel;
    QPlainTextEdit* m_potaConsole;

#ifdef HAVE_WEBSOCKETS
    // FreeDV tab — connection controls
    QPushButton*    m_freedvStartBtn{nullptr};
    QPushButton*    m_freedvAutoStartBtn{nullptr};
    QLabel*         m_freedvStatusLabel{nullptr};
    QPlainTextEdit* m_freedvConsole{nullptr};
    // FreeDV tab — station reporting controls
    QCheckBox*      m_fdvReportCheck{nullptr};
    QLineEdit*      m_fdvCallsignEdit{nullptr};
    QCheckBox*      m_fdvUseRadioCallsignCheck{nullptr};
    QLineEdit*      m_fdvGridEdit{nullptr};
    QCheckBox*      m_fdvUseGpsCheck{nullptr};
    QLineEdit*      m_fdvMessageEdit{nullptr};
#endif

    QPushButton*    m_wsjtxColorCQ;
    QPushButton*    m_wsjtxColorPOTA;
    QPushButton*    m_wsjtxColorCallingMe;
    QPushButton*    m_wsjtxColorDefault;

    // Spot batching (1/sec flush)
    QVector<DxSpot>        m_spotBatch;
    QTimer*                m_spotBatchTimer{nullptr};
    void flushSpotBatch();

    // Spot list tab
    SpotTableModel*        m_spotModel;
    QTableView*            m_spotTable;
    BandFilterProxy*       m_proxyModel;

    // Display tab
    QLabel*            m_totalSpotsLabel{nullptr};
    QLabel*            m_dxccStatsLabel{nullptr};
    DxccColorProvider* m_dxccProvider{nullptr};
};

} // namespace AetherSDR
