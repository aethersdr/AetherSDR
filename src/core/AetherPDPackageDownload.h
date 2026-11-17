#pragma once
#include "AetherPDPackage.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <memory>
#include <optional>
#include <variant>

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
// A fresh install has its own pins and publisher key, with no predecessor.
// Only the implemented ARM64/profile-1 first-install protocol is selectable.
struct AetherPDFirstRelease {
    QString version;
    QString firmware;
    quint32 profile{0};
    std::array<QUrl, 8> urls;
    AetherPDFirstPackage::Trust trust;
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
    bool start(const AetherPDFirstRelease& release);
    void cancel();
    bool active() const { return m_active; }
    std::optional<AetherPDPackage> package() const { return m_package; }
    std::optional<AetherPDFirstPackage> firstPackage() const { return m_firstPackage; }
signals:
    void progress(qint64 received, qint64 total);
    void ready();
    void failed(AetherSDR::AetherPDPackageDownload::Failure failure);
private:
    using Selection = std::variant<AetherPDRelease, AetherPDFirstRelease>;
    bool begin(const Selection& release);
    const std::array<QUrl, 8>& urls() const;
    const std::array<AetherPDPackage::Pin, 8>& pins() const;
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
    Selection m_release;
    AetherPDPackage::Files m_files;
    std::optional<AetherPDPackage> m_package;
    std::optional<AetherPDFirstPackage> m_firstPackage;
    std::size_t m_index{0};
    qint64 m_received{0}, m_total{0};
    bool m_active{false};
};
} // namespace AetherSDR
