#include "core/backends/hl2/Hl2Receivers.h"

namespace AetherSDR::hl2 {

namespace {
// The pan-id prefix. "hl2-0" rather than the bare "hl2" the single-receiver
// backend used: a scheme where the first receiver keeps an unsuffixed id and
// the rest gain one gives two spellings for receiver 0, and every lookup then
// has to remember both.
constexpr QLatin1String kPanPrefix("hl2-");
}  // namespace

QString hl2PanId(int uiNumber)
{
    return kPanPrefix + QString::number(uiNumber);
}

std::optional<int> hl2PanNumber(const QString& panId)
{
    if (!panId.startsWith(kPanPrefix))
        return std::nullopt;
    bool ok = false;
    const int n = QStringView(panId).mid(kPanPrefix.size()).toInt(&ok);
    if (!ok || n < 0)
        return std::nullopt;
    return n;
}

int hl2RoleAfterRemove(int role, int removedDdc)
{
    if (role == removedDdc)
        return -1;          // gone; the caller picks a new home
    return role > removedDdc ? role - 1 : role;
}

void Hl2ReceiverMap::reset(int count)
{
    m_rx.clear();
    m_nextUiNumber = 0;   // a new session starts UI numbering over
    if (count < 0)
        count = 0;
    m_rx.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        Hl2ReceiverIds ids;
        ids.ddcIndex = i;
        ids.uiNumber = i;
        ids.panId = hl2PanId(i);
        // dspChannel and analyzerId stay -1: they are not known until the DSP
        // opens, and defaulting them to `i` is exactly the arithmetic this
        // whole type exists to prevent.
        m_rx.push_back(std::move(ids));
    }
    // Leave the counter past everything just issued, so a later append() cannot
    // hand out a UI number that is already in use. Missing this made the first
    // appended receiver collide with receiver 0.
    m_nextUiNumber = count;
}

const Hl2ReceiverIds* Hl2ReceiverMap::byDdc(int ddcIndex) const
{
    for (const auto& r : m_rx)
        if (r.ddcIndex == ddcIndex)
            return &r;
    return nullptr;
}

const Hl2ReceiverIds* Hl2ReceiverMap::byUi(int uiNumber) const
{
    for (const auto& r : m_rx)
        if (r.uiNumber == uiNumber)
            return &r;
    return nullptr;
}

const Hl2ReceiverIds* Hl2ReceiverMap::byDspChannel(int dspChannel) const
{
    if (dspChannel < 0)
        return nullptr;   // -1 is "not open yet" and must not match an unopened peer
    for (const auto& r : m_rx)
        if (r.dspChannel == dspChannel)
            return &r;
    return nullptr;
}

const Hl2ReceiverIds* Hl2ReceiverMap::byPanId(const QString& panId) const
{
    const auto n = hl2PanNumber(panId);
    return n ? byUi(*n) : nullptr;
}

Hl2ReceiverIds* Hl2ReceiverMap::mutableByDdc(int ddcIndex)
{
    for (auto& r : m_rx)
        if (r.ddcIndex == ddcIndex)
            return &r;
    return nullptr;
}

int Hl2ReceiverMap::append()
{
    Hl2ReceiverIds ids;
    ids.ddcIndex = static_cast<int>(m_rx.size());   // contiguous, as the gateware needs
    ids.uiNumber = m_nextUiNumber++;                // monotonic, never reused
    ids.panId = hl2PanId(ids.uiNumber);
    // dspChannel and analyzerId stay -1 until the DSP actually opens.
    m_rx.push_back(std::move(ids));
    return m_rx.back().ddcIndex;
}

bool Hl2ReceiverMap::remove(int ddcIndex)
{
    for (std::size_t i = 0; i < m_rx.size(); ++i) {
        if (m_rx[i].ddcIndex != ddcIndex)
            continue;
        m_rx.erase(m_rx.begin() + static_cast<std::ptrdiff_t>(i));
        // Close the gap in the DDC space ONLY. Every receiver after the removed
        // one moves down one hardware slot, because that index is its position
        // in the EP6 round and the gateware streams numRx contiguous receivers.
        // Their uiNumber and panId are untouched, so the panes the operator
        // still has open keep their identity.
        for (std::size_t k = i; k < m_rx.size(); ++k)
            m_rx[k].ddcIndex = static_cast<int>(k);
        return true;
    }
    return false;
}

}  // namespace AetherSDR::hl2
