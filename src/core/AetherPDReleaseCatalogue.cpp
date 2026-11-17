#include "AetherPDReleaseCatalogue.h"
#include <QChar>
#include <utility>

namespace AetherSDR {
AetherPDReleaseCatalogue::AetherPDReleaseCatalogue(std::vector<First> first)
    : m_first(std::move(first))
{
    if (m_first.size() > 32) { return; }
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
} // namespace AetherSDR
