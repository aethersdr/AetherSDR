#pragma once

#include <QByteArray>
#include <QString>

namespace AetherSDR {

// Whether a CTR2-Max is registered with its maker's firmware: Reg.txt in the
// stock firmware's LittleFS volume (the "spiffs" partition), as CTR2-Max
// firmware v1.02.00m and later write it (see ctr2max_reg.h). AetherCTR2 is
// installed only on registered units, as agreed with Lynovation, and checks
// the same itself at every start. Read-only: see ctr2max_reg.h.
namespace Ctr2MaxRegistration {

inline constexpr quint32 kOffset = 0x310000;   // the partition, in flash
inline constexpr quint32 kSize = 0xE0000;

enum class Result { Registered, NotRegistered, Unreadable };

// partition: the volume's bytes (kSize of them, read from kOffset). On
// Registered, call gets Reg.txt's first line (the hashed call). Safe from any
// thread; calls are serialized, because the C reader's state is static.
Result check(const QByteArray& partition, QString* call = nullptr);

} // namespace Ctr2MaxRegistration
} // namespace AetherSDR
