#pragma once
#include "AetherPDPackage.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <memory>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
namespace AetherSDR {
// Selected by trusted application code, never a manifest from the download.
// Shipping entries require qualified release pins and independently held keys.
struct AetherPDRelease {
    QString version;
    QString firmware;
    quint32 profile{0};
    std::array<QUrl, 8> urls;
    AetherPDPackage::Trust trust;
    bool valid() const;
};

// Downloads fixed pinned files into bounded RAM. No credentials, disk writes,
// executable launch, radio access or installation approval. Reuse after cancel
// starts a new download; signals from a retired reply cannot finish that job.
class AetherPDPackageDownload final : public QObject {
    Q_OBJECT
public:
    explicit AetherPDPackageDownload(QObject* parent = nullptr);
    AetherPDPackageDownload(QNetworkAccessManager& network, QObject* parent = nullptr);
    ~AetherPDPackageDownload() override;
    enum class Failure { Network, Oversize, Integrity, Timeout };
    Q_ENUM(Failure)
    bool start(const AetherPDRelease& release);
    void cancel();
    bool active() const { return m_active; }
    std::optional<AetherPDPackage> package() const { return m_package; }
signals:
    void progress(qint64 received, qint64 total);
    void ready();
    void failed(AetherSDR::AetherPDPackageDownload::Failure failure);
private:
    void initialize();
    void next();
    bool consume();
    void finished();
    void fail(Failure failure);
    void retireReply();
    std::unique_ptr<QNetworkAccessManager> m_owned;
    QPointer<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_deadline;
    AetherPDRelease m_release;
    AetherPDPackage::Files m_files;
    std::optional<AetherPDPackage> m_package;
    std::size_t m_index{0};
    qint64 m_received{0}, m_total{0};
    bool m_active{false};
};
} // namespace AetherSDR
