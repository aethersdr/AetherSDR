#pragma once
#include <QtGlobal>

namespace AetherSDR {
// ExpertSDR3 TCI v2.0 Stream: 16 native-endian uint32 fields (64 bytes),
// followed by sample payload. Shared by the worker and codec tests.
struct TciAudioHeader {
    quint32 receiver;     // receiver/TRX number
    quint32 sampleRate;   // Hz
    quint32 format;       // 0=int16, 1=int24, 2=int32, 3=float32
    quint32 codec;        // 0 (uncompressed)
    quint32 crc;          // 0 (unused)
    quint32 length;       // number of real samples in data
    quint32 type;         // 0=IQ, 1=RX_AUDIO, 2=TX_AUDIO, 3=TX_CHRONO
    quint32 channels;     // 1 or 2
    quint32 reserved[8];  // zero-filled
};
static_assert(sizeof(TciAudioHeader) == 64, "TCI audio header must be 64 bytes");

} // namespace AetherSDR
