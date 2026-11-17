#pragma once
#include "AetherPDPackageDownload.h"
#include <QByteArray>
#include <QString>
#include <optional>
#include <vector>

namespace AetherSDR {
// Application-owned trust data, compiled into the signed desktop application.
// Model names are exact backend identity values, never inferred from firmware.
// Retained entries may recover a saved attempt without offering a fresh install.
class AetherPDReleaseCatalogue {
public:
    struct First {
        QString model;
        AetherPDFirstRelease release;
        QByteArray candidateDigest; // Raw BLAKE2b-256 of the signed release.
        bool offer{false};
    };
    explicit AetherPDReleaseCatalogue(std::vector<First> first = {});
    bool valid() const { return m_valid; }
    bool empty() const { return m_first.empty(); }
    std::optional<First> first(const QString& model, const QString& firmware) const;
    std::vector<First> retainedFirst(const QString& model, const QString& firmware) const;
    static AetherPDReleaseCatalogue shipping();
private:
    std::vector<First> m_first;
    bool m_valid{false};
};
} // namespace AetherSDR
