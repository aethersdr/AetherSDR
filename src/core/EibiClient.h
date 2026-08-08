#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QDateTime>
#include <QVector>
#include <QString>
#include <atomic>

#include "DxClusterClient.h" // for DxSpot

namespace AetherSDR {

struct EibiEntry {
    double  freqMhz{0.0};
    int     startMins{0}; // Start time in UTC minutes (0..1440)
    int     endMins{0};   // End time in UTC minutes (0..1440)
    QString days;        // e.g. "Mo-Fr", "1245", "SaSu", empty = daily
    QString itu;         // ITU country code
    QString station;     // Station name
    QString lang;        // Language code
    QString target;      // Target area
    QString remarks;     // Remarks / notes / site code
};

class EibiClient : public QObject {
    Q_OBJECT

public:
    explicit EibiClient(QObject* parent = nullptr);
    ~EibiClient() override;

    static QString cacheFilePath();
    static QString lookupCountry(const QString& code);
    static QString lookupLanguage(const QString& code);
    static QString lookupTarget(const QString& code);
    static QString lookupSite(const QString& countryCode, const QString& siteCode);
    static QString formatDays(const QString& daysCode);
    static QString formatTime(int startMins, int endMins);

    bool isEnabled() const { return m_enabled; }
    QDateTime cacheLastModified() const;
    QDateTime nextFetchTime() const;

public slots:
    void initialize();
    void setEnabled(bool enabled);
    void checkForUpdates(bool forceRefresh = false);
    void forceUpdate();
    void updateActiveSpots();

    bool parseContent(const QString& content);

signals:
    void spotsUpdated(const QVector<DxSpot>& spots);
    void fetchFailed(const QString& errorMessage);

private slots:
    void onDownloadFinished(QNetworkReply* reply);

private:
    void loadAndParseCache();
    bool isEntryActive(const EibiEntry& entry, const QDateTime& utcNow) const;
    bool isDayActive(const QString& daysStr, int dayOfWeek) const;

    QNetworkAccessManager* m_nam{nullptr};
    QTimer*               m_updateTimer{nullptr};
    QVector<EibiEntry>    m_entries;
    std::atomic<bool>     m_enabled{false};
    bool                  m_isDownloading{false};

    // Note: eibispace.de uses an expired/mismatched SSL cert (*.evanzo-server.de) on HTTPS port 443,
    // so HTTP is required for downloads. All remote strings are HTML-escaped via toHtmlEscaped().
    static constexpr const char* kSourceUrl = "http://www.eibispace.de/dx/eibi.txt";
    static constexpr qint64 kMaxDownloadBytes = 5 * 1024 * 1024; // 5 MB size cap
    static constexpr qint64 kMaxCacheAgeSec = 7 * 86400;          // 7 days
};

} // namespace AetherSDR
