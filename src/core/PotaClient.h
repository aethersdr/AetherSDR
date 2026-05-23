#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QFile>
#include <QSet>
#include <atomic>
#include "DxClusterClient.h"  // for DxSpot

namespace MasterSDR {

// POTA (Parks on the Air) spot client — polls https://api.pota.app/spot/activator
// every 30 seconds for active activations. Emits spotReceived() for each new spot.
class PotaClient : public QObject {
    Q_OBJECT

public:
    explicit PotaClient(QObject* parent = nullptr);
    ~PotaClient() override;

    void startPolling(int intervalSec = 30);
    void stopPolling();
    bool isPolling() const { return m_polling; }

    QString logFilePath() const;

public slots:
    // Defer QNetworkAccessManager + timer construction to the worker thread (#1929) —
    // see DxClusterClient::initialize().
    void initialize();

signals:
    void started();
    void stopped();
    void pollError(const QString& error);
    void spotReceived(const DxSpot& spot);
    void rawLineReceived(const QString& line);
    void pollComplete(int spotCount, int newCount);

private slots:
    void onPollTimer();

private:
    QNetworkAccessManager* m_nam{nullptr};
    QTimer*     m_pollTimer{nullptr};
    QFile       m_logFile;
    QSet<int>   m_seenSpotIds;   // track spotId to only emit new spots
    std::atomic<bool> m_polling{false};

    static constexpr const char* ApiUrl = "https://api.pota.app/spot/activator";
};

} // namespace MasterSDR
