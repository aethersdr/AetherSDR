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

void Hl2ReceiverMap::reset(int count)
{
    m_rx.clear();
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

}  // namespace AetherSDR::hl2
