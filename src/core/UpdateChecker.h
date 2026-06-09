#pragma once

#include <QNetworkAccessManager>
#include <QObject>

namespace AetherSDR {

// Checks the GitHub releases API for a newer AetherSDR version.
// kReleasesPageUrl is the browser-facing page; the API endpoint lives in the .cpp.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    static constexpr char kReleasesPageUrl[] =
        "https://github.com/aethersdr/AetherSDR/releases/latest";

    explicit UpdateChecker(QObject* parent = nullptr);
    void checkNow();

signals:
    void updateAvailable(const QString& latestVersion);
    void upToDate(const QString& currentVersion);
    void checkFailed();

private:
    QNetworkAccessManager m_nam;
    bool m_inFlight = false;
};

} // namespace AetherSDR
