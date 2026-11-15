#pragma once
#include "models/Kat500Types.h"
#include <functional>
#include <utility>

namespace AetherSDR::Kat500 {
// Official September 2023 serial reference and Utility 1.23.11.9 captures.
// Frames remain case-sensitive because lower-case kat500 identifies the loader.
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
QByteArray configurationQuery(SettingKey key);
QByteArray configurationSet(SettingKey key, double value);
std::optional<double> configurationValue(SettingKey key, const QByteArray& frame);
QString validateConfiguration(const Configuration& confirmed, const Configuration& edits);
QByteArray buildFrequency(int khz);
QByteArray buildErase(int band, int antenna);
bool matchesStatusQuery(const QByteArray& query, const QByteArray& frame);
std::optional<MemoryBin> decodeMemoryBin(const QList<QByteArray>& frames);
} // namespace AetherSDR::Kat500
