#pragma once

#include "RadioControlTypes.h"
#include "DaxTxStream.h"

#include <QMetaType>
#include <QSet>
#include <optional>

namespace AetherSDR {

using AudioControlGuard = DaxTxRequestGuard;

enum class AudioStreamKind { RemoteReceive, RemoteTransmit, DaxReceive, DaxTransmit, Iq };
enum class AudioStreamAction { Create, Remove, SetIqRate };
enum class AudioCompression { None, Opus };

// Allocation and configuration convey no stream selection or keying authority.
struct AudioStreamRequest {
    AudioStreamKind kind{AudioStreamKind::RemoteReceive};
    AudioStreamAction action{AudioStreamAction::Create};
    quint32 streamId{0};
    int channel{0};
    int sampleRate{0};
    AudioCompression compression{AudioCompression::None};
    AudioControlGuard guard{};

    bool valid() const
    {
        if (kind < AudioStreamKind::RemoteReceive || kind > AudioStreamKind::Iq
            || compression < AudioCompression::None || compression > AudioCompression::Opus) { return false; }
        if (action == AudioStreamAction::Remove) { return streamId != 0; }
        if (action == AudioStreamAction::SetIqRate) {
            return kind == AudioStreamKind::Iq && streamId != 0 && channel >= 1 && channel <= 4
                && (sampleRate == 24000 || sampleRate == 48000 || sampleRate == 96000 || sampleRate == 192000);
        }
        if (action != AudioStreamAction::Create || streamId != 0) { return false; }
        if (kind == AudioStreamKind::Iq) { return channel >= 1 && channel <= 4; }
        if (kind == AudioStreamKind::DaxReceive) { return channel >= 1 && channel <= 8; }
        return channel == 0;
    }
};

struct AudioStreamControl {
    QSet<AudioStreamKind> kinds;
    bool supports(AudioStreamKind kind) const { return kinds.contains(kind); }
};
struct AudioStreamReply {
    RadioControlReply result;
    quint32 streamId{0};
};
struct AudioStreamDelta {
    quint32 streamId{0};
    bool removed{false};
    bool invalidOwner{false};
    std::optional<AudioStreamKind> kind;
    std::optional<quint32> clientHandle;
    std::optional<int> channel;
};
using AudioStreamCompletion = std::function<void(const AudioStreamReply&)>;

struct GraphicEqRequest {
    enum class Path { Receive, Transmit };
    enum class Field { Enabled, BandGain };
    Path path{Path::Receive};
    Field field{Field::Enabled};
    int band{0};
    int value{0};
    AudioControlGuard guard{};
    bool valid() const
    {
        if (path != Path::Receive && path != Path::Transmit) { return false; }
        if (field == Field::Enabled) { return value == 0 || value == 1; }
        return field == Field::BandGain && band >= 0 && band < 8 && value >= -10 && value <= 10;
    }
};
struct GraphicEqControl {
    QSet<GraphicEqRequest::Path> paths;
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::AudioStreamRequest)
Q_DECLARE_METATYPE(AetherSDR::GraphicEqRequest)
Q_DECLARE_METATYPE(AetherSDR::AudioStreamDelta)
