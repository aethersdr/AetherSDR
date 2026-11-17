#pragma once
#include "AetherPDCarrierImage.h"
#include <QByteArray>
#include <QString>
#include <array>
#include <optional>

namespace AetherSDR {
class AetherPDInstallationCoordinator;
// Pins come from the application's authenticated release catalogue, never a
// manifest or key alongside the package being loaded. No network or execution.
class AetherPDPackage {
public:
    struct Pin { qint64 size{0}; QByteArray sha256; }; // Raw 32-byte SHA-256.
    struct Trust {
        std::array<Pin, 8> files;
        QByteArray candidateKey, previousKey; // Independently trusted Ed25519 keys.
    };
    using Files = std::array<QByteArray, 8>;
    static std::array<QString, 8> fileNames();
    static std::array<qint64, 8> fileSizeLimits();
    static bool validTrust(const Trust& trust);
    static std::optional<AetherPDPackage> verify(const Files& files, const Trust& trust);
    static std::optional<AetherPDPackage> load(const QString& directory, const Trust& trust);
    quint32 profile() const { return m_profile; }
    QString firmware() const { return m_firmware; }
    // A public target plan, not operator consent. Serial is supplied by the
    // connected radio; worker/source and release keys came from verified bytes.
    QByteArray plan(const QString& serial) const;
private:
    friend class AetherPDInstallationCoordinator;
    QByteArray m_worker, m_candidate, m_previous;
    QByteArray m_candidateKey, m_previousKey;
    aetherpd::CarrierExecutables m_code;
    QByteArray m_licenses;
    quint32 m_profile{0};
    QString m_firmware;
};
// Fresh installation has no predecessor release/key. All pins and the publisher
// key must come from the authenticated application catalogue, outside the inputs.
class AetherPDFirstPackage {
public:
    struct Trust {
        std::array<AetherPDPackage::Pin, 8> files;
        QByteArray publisherKey;
    };
    using Files = std::array<QByteArray, 8>;
    struct Uploads {
        QByteArray plan;
        QByteArray publisherKey;
        QString nativeName;
        QByteArray nativeArchive;
        aetherpd::CarrierImage carrier;
    };
    static std::array<QString, 8> fileNames();
    static std::array<qint64, 8> fileSizeLimits();
    static bool validTrust(const Trust& trust);
    static std::optional<AetherPDFirstPackage> verify(const Files& files, const Trust& trust);
    static std::optional<AetherPDFirstPackage> load(const QString& directory, const Trust& trust);
    QString firmware() const { return m_firmware; }
    // Local preparation only, before exact-plan consent. Call off the UI thread.
    // Secret generation/storage belongs to the caller; only its hash enters ZIP.
    std::optional<Uploads> prepare(const QString& serial, const QByteArray& tokenHash,
                                  const QString& hostname, const QByteArray& installationId) const;
    QByteArray candidateDigest() const;
private:
    AetherPDFirstPackage() = default;
    Files m_files;
    QByteArray m_key;
    QString m_firmware;
};
} // namespace AetherSDR
