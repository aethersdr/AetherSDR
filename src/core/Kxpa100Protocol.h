#pragma once
#include "models/Kxpa100Types.h"
#include <functional>
#include <utility>

namespace AetherSDR::Kxpa100 {
class FrameParser {
public:
    void setCallback(std::function<void(const QByteArray&)> callback) { m_callback = std::move(callback); }
    void feed(const QByteArray& bytes);
    void reset() { m_buffer.clear(); m_discard = false; }
private:
    QByteArray m_buffer;
    bool m_discard = false;
    std::function<void(const QByteArray&)> m_callback;
};
bool applyStatus(const QByteArray& frame, Status& status);
bool matchesStatusQuery(const QByteArray& query, const QByteArray& frame);
QByteArray buildFrequencyCommand(const QByteArray& command, int khz);
QByteArray buildErase(int band, int antenna);
QByteArray configurationSet(int band, int mask);
std::optional<Configuration> parseConfiguration(const QByteArray& frame);
std::optional<MemoryBin> decodeMemoryBin(const QByteArray& frame);
}
