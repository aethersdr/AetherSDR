#include "AetherPDReleaseCatalogue.h"
#include <QChar>
#include <utility>

namespace AetherSDR {
AetherPDReleaseCatalogue::AetherPDReleaseCatalogue(std::vector<First> first, std::vector<Upgrade> upgrades)
    : m_first(std::move(first)), m_upgrades(std::move(upgrades))
{
    if (m_first.size() + m_upgrades.size() > 32) { return; }
    for (std::size_t i = 0; i < m_first.size(); ++i) {
        const First& entry = m_first[i];
        if (entry.model.isEmpty() || entry.model.size() >= 64 || entry.model.contains(QChar::Null)
            || entry.model != entry.model.trimmed() || !entry.release.valid()
            || entry.candidateDigest.size() != 32 || entry.candidateDigest == QByteArray(32, '\0')) { return; }
        for (std::size_t j = 0; j < i; ++j) {
            const First& previous = m_first[j];
            if (entry.model != previous.model || entry.release.firmware != previous.release.firmware) { continue; }
            if ((entry.offer && previous.offer)
                || (entry.candidateDigest == previous.candidateDigest
                    && entry.release.trust.publisherKey == previous.release.trust.publisherKey)) { return; }
        }
    }
    for (std::size_t i = 0; i < m_upgrades.size(); ++i) {
        const Upgrade& entry = m_upgrades[i];
        if (entry.model.isEmpty() || entry.model.size() >= 64 || entry.model.contains(QChar::Null)
            || entry.model != entry.model.trimmed() || !entry.release.valid()
            || entry.previousDigest.size() != 32 || entry.previousDigest == QByteArray(32, '\0')
            || entry.candidateDigest.size() != 32 || entry.candidateDigest == QByteArray(32, '\0')
            || entry.previousDigest == entry.candidateDigest) { return; }
        for (std::size_t j = 0; j < i; ++j) {
            const Upgrade& previous = m_upgrades[j];
            if (entry.model == previous.model && entry.release.firmware == previous.release.firmware
                && entry.release.profile == previous.release.profile && entry.previousDigest == previous.previousDigest
                && entry.release.trust.previousKey == previous.release.trust.previousKey) { return; }
        }
    }
    m_valid = true;
}
std::optional<AetherPDReleaseCatalogue::First> AetherPDReleaseCatalogue::first(const QString& model, const QString& firmware) const
{
    if (!m_valid) { return std::nullopt; }
    for (const First& entry : m_first) {
        if (entry.offer && entry.model == model && entry.release.firmware == firmware) { return entry; }
    }
    return std::nullopt;
}
std::vector<AetherPDReleaseCatalogue::First> AetherPDReleaseCatalogue::retainedFirst(const QString& model, const QString& firmware) const
{
    std::vector<First> result;
    if (m_valid) {
        for (const First& entry : m_first) {
            if (entry.model == model && entry.release.firmware == firmware) { result.push_back(entry); }
        }
    }
    return result;
}
AetherPDReleaseCatalogue AetherPDReleaseCatalogue::shipping()
{
    // No release has completed the hardware, RF and independent release gates.
    // Add reviewed pins/keys here only after those gates; never load local files
    // or adjacent download manifests to establish publisher trust.
    return AetherPDReleaseCatalogue{};
}
bool AetherPDReleaseCatalogue::hasUpgrades(const QString& model, const QString& firmware) const
{
    if (m_valid) {
        for (const Upgrade& entry : m_upgrades) {
            if (entry.model == model && entry.release.firmware == firmware) { return true; }
        }
    }
    return false;
}
std::optional<AetherPDReleaseCatalogue::Upgrade> AetherPDReleaseCatalogue::upgrade(
    const QString& model, const aetherpd::InstallationObservation& installed) const
{
    if (m_valid) {
        for (const Upgrade& entry : m_upgrades) {
            if (entry.model == model && entry.release.firmware == installed.firmware && entry.release.profile == installed.profile
                && QString::fromLatin1(entry.previousDigest.toHex()) == installed.selectedDigest
                && QString::fromLatin1(entry.release.trust.previousKey.toHex()) == installed.selectedKey) { return entry; }
        }
    }
    return std::nullopt;
}
} // namespace AetherSDR
