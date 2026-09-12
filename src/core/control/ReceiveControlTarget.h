#pragma once

#include "ControlProtocolCodec.h"

#include <QObject>
#include <QString>
#include <memory>
#include <array>
#include <utility>

namespace AetherSDR {
class RadioModel;
namespace control {
class RadioConnectionTarget;

enum class ReceiveOperation { Mode, Filter, AudioGain, AudioMute, PanCenter, PanBandwidth };
inline constexpr std::array<std::pair<ReceiveOperation, const char*>, 6> kReceiveMethods{{
    {ReceiveOperation::Mode, "slice.setMode"},
    {ReceiveOperation::Filter, "slice.setFilter"},
    {ReceiveOperation::AudioGain, "slice.setAudioGain"},
    {ReceiveOperation::AudioMute, "slice.setAudioMute"},
    {ReceiveOperation::PanCenter, "panadapter.setCenter"},
    {ReceiveOperation::PanBandwidth, "panadapter.setBandwidth"},
}};

// Closed typed intent set; no reflection, raw commands or runtime setter names.
class ReceiveControlTarget : public QObject {
public:
    using QObject::QObject;
    [[nodiscard]] virtual bool available(ReceiveOperation operation) const = 0;
    [[nodiscard]] virtual std::optional<ProtocolError> setMode(int slice, const QString& mode) = 0;
    [[nodiscard]] virtual std::optional<ProtocolError> setFilter(int slice, int lowHz, int highHz) = 0;
    [[nodiscard]] virtual std::optional<ProtocolError> setAudioGain(int slice, int gain) = 0;
    [[nodiscard]] virtual std::optional<ProtocolError> setAudioMute(int slice, bool muted) = 0;
    [[nodiscard]] virtual std::optional<ProtocolError> setPanCenter(const QString& pan, qint64 hz) = 0;
    [[nodiscard]] virtual std::optional<ProtocolError> setPanBandwidth(const QString& pan, qint64 hz) = 0;
};

[[nodiscard]] std::unique_ptr<ReceiveControlTarget> makeModelReceiveControlTarget(
    RadioModel* radio, RadioConnectionTarget* connection);

} // namespace control
} // namespace AetherSDR
