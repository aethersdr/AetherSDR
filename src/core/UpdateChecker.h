#pragma once

#include <QNetworkAccessManager>
#include <QObject>

namespace AetherSDR {

// Checks the GitHub releases API for a newer AetherSDR version.
// All network errors are swallowed silently — no UI noise on failure.
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

private:
    QNetworkAccessManager m_nam;
};

} // namespace AetherSDR
